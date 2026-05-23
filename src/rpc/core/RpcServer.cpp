#include "novanet/rpc/core/RpcServer.h"

#include <google/protobuf/service.h>

#include <string>
#include <utility>
#include <vector>

#include "novanet/base/Logger.h"
#include "novanet/net/TcpServer.h"
#include "novanet/rpc/protocol/FrameType.h"

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
            this->handleMessage(conn, *buffer);
        });
}

}  // namespace novanet::rpc