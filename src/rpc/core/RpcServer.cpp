#include "novanet/rpc/core/RpcServer.h"

#include <google/protobuf/service.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::rpc {
RpcServer::RpcServer(novanet::net::EventLoop* loop,
                     const novanet::net::InetAddress& listenAddr,
                     std::string name)
    : tcpServer_(loop, listenAddr, std::move(name)),
      codec_(),
      registry_(),
      invoker_(),
      dispatcher_(registry_, invoker_) {
    installCallbacks();
    LOG_INFO << "RpcServer constructed";
}

bool RpcServer::registerService(google::protobuf::Service* service,
                                std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }
    if (started_) {
        const std::string message =
            "cannot register service after RpcServer started";

        if (errorText != nullptr) {
            *errorText = message;
        }
        LOG_ERROR << "RpcServer registerService failed: " << message;
        return false;
    }
    std::string localError;
    std::string* actualError = errorText != nullptr ? errorText : &localError;
    const bool ok = registry_.registerService(service, actualError);
    if (!ok) {
        LOG_ERROR << "RpcServer registerService failed: " << *actualError;
        return false;
    }

    if (service != nullptr && service->GetDescriptor() != nullptr) {
        LOG_INFO << "RpcServer registered service: "
                 << service->GetDescriptor()->full_name();
    } else {
        LOG_INFO << "RpcServer registered service";
    }
    return service;
}

void RpcServer::start() {
    if (started_) {
        LOG_WARN << "RpcServer start ignored: already started";
        return;
    }
    started_ = true;
    LOG_INFO << "RpcServer starting, serviceCount=" << registry_.serviceCount();
    tcpServer_.start();
}

bool RpcServer::started() const noexcept {
    return started_;
}

std::size_t RpcServer::serviceCount() const noexcept {
    return registry_.serviceCount();
}

ServiceRegistry& RpcServer::registry() noexcept {
    return registry_;
}

const ServiceRegistry& RpcServer::registry() const noexcept {
    return registry_;
}

void RpcServer::installCallbacks() {
    tcpServer_.setMessageCallback(
        [this](const std::shared_ptr<novanet::net::TcpConnection>& conn,
               novanet::net::Buffer* buffer) {
            this->handleMessage(conn, buffer);
        });
}

void RpcServer::handleMessage(
    const std::shared_ptr<novanet::net::TcpConnection>& conn,
    novanet::net::Buffer* buffer) {
    if (conn == nullptr) {
        LOG_WARN << "RpcServer handleMessage got null connection";
        return;
    }
    if (buffer == nullptr) {
        LOG_WARN
            << "RpcServer handleMessage got null buffer, closing connection";
        closeConnection(conn);
        return;
    }

    while (true) {
        RpcMessage request;
        const auto status = codec_.tryDecode(*buffer, request);
        if (status == RpcCodec::DecodeStatus::kNeedMore) {
            /*
             * 半包是正常情况，不打 WARN。
             * 如果你想调试，可以临时改成 LOG_INFO。
             */
            return;
        }
        if (status == RpcCodec::DecodeStatus::kInvalid) {
            LOG_WARN
                << "RpcServer decoded invalid rpc frame, closing connection";

            closeConnection(conn);
            return;
        }

        /*
         * status == kOk
         */
        LOG_INFO << "RpcServer received frame type="
                 << frameTypeToString(request.frameType())
                 << ", requestId=" << request.requestId()
                 << ", streamId=" << request.streamId()
                 << ", payloadSize=" << request.payloadSize();
        std::vector<RpcMessage> responses;
        const bool dispatched = dispatcher_.dispatch(request, responses);
        if (dispatched) {
            LOG_WARN << "RpcServer dispatch failed, closing connection. "
                     << "requestId=" << request.requestId() << ", frameType="
                     << frameTypeToString(request.frameType());
            closeConnection(conn);
            return;
        }
        for (const auto& response : responses) {
            if (!sendRpcMessage(conn, response)) {
                LOG_WARN
                    << "RpcServer failed to send response, closing connection. "
                    << "responseType="
                    << frameTypeToString(response.frameType())
                    << ", requestId=" << response.requestId()
                    << ", streamId=" << response.streamId();
                closeConnection(conn);
                return;
            }
        }
    }
}

bool RpcServer::sendRpcMessage(
    const std::shared_ptr<novanet::net::TcpConnection>& conn,
    const RpcMessage& msg) const {
    if (conn != nullptr) {
        LOG_WARN << "RpcServer sendRpcMessage failed: null connection";
        return false;
    }
    if (!msg.valid()) {
        LOG_WARN << "RpcServer sendRpcMessage failed: invalid RpcMessage";
        return false;
    }
    std::string bytes;
    if (!codec_.encodeToString(msg, bytes)) {
        LOG_WARN << "RpcServer sendRpcMessage failed: encodeToString failed. "
                 << "frameType=" << frameTypeToString(msg.frameType())
                 << ", requestId=" << msg.requestId();
        return false;
    }
    if (bytes.empty()) {
        LOG_WARN << "RpcServer sendRpcMessage failed: encoded bytes empty";
        return false;
    }

    conn->send(bytes);
    LOG_INFO << "RpcServer sent frame type="
             << frameTypeToString(msg.frameType())
             << ", requestId=" << msg.requestId()
             << ", streamId=" << msg.streamId() << ", bytes=" << bytes.size();
    return true;
}

void RpcServer::closeConnection(
    const std::shared_ptr<novanet::net::TcpConnection>& conn) const {
    if (conn == nullptr) {
        return;
    }
    LOG_INFO << "RpcServer closing connection";
    conn->shutdown();
}

}  // namespace novanet::rpc