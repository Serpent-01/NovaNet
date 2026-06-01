#include "novanet/rpc/core/RpcChannel.h"

#include <chrono>
#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {
using namespace meta;
RpcChannel::RpcChannel(std::shared_ptr<net::TcpConnection> conn)
    : connection_(std::move(conn)) {
}

RpcChannel::~RpcChannel() {
    onConnectionClosed();
}

bool RpcChannel::callUnary(const std::string& service,
                           const std::string& method,
                           const google::protobuf::Message& request,
                           google::protobuf::Message& response, int timeoutMs) {
    if (!connection_) {
        LOG_WARN << "RpcChannel::callUnary failed: null connection";
        return false;
    }
    if (service.empty()) {
        LOG_WARN << "RpcChannel::callUnary failed: empty service";
        return false;
    }
    if (method.empty()) {
        LOG_WARN << "RpcChannel::callUnary failed: empty method";
        return false;
    }
    if (timeoutMs <= 0) {
        LOG_WARN << "RpcChannel::callUnary failed: invalid timeoutMs="
                 << timeoutMs;
        return false;
    }

    const std::uint64_t requestId = nextRequestId();
    auto call = pendingCalls_.create(requestId);
    if (!call) {
        LOG_ERROR << "RpcChannel::callUnary failed: duplicated or invalid "
                  << "requestId=" << requestId;
        return false;
    }
    RpcMessage requestMessage;
    if (!buildUnaryRequestMessage(service, method, request, requestId,
                                  &requestMessage)) {
        static_cast<void>(pendingCalls_.remove(requestId));
        static_cast<void>(
            call->markFailed("failed to build unary request message"));
        return false;
    }
    if (!sendMessage(requestMessage)) {
        static_cast<void>(pendingCalls_.remove(requestId));
        static_cast<void>(call->markFailed("failed to send unary request"));
        return false;
    }
    /*
     * 这里开始同步等待响应。
     *
     * 正常情况：
     *   onMessage() -> handleUnaryResponse()
     *   -> pendingCalls_.complete()
     *   -> call->markDone()
     *   -> waitFor() 被唤醒
     *
     * 超时情况：
     *   waitFor() 自己把 call 标记为 kTimeout
     */
    const auto state = call->waitFor(std::chrono::milliseconds{timeoutMs});
    /*
     * 清理 PendingCallManager 中的映射。
     *
     * 这里允许 remove 不到：
     *
     * 1. response 先到：
     *    handleUnaryResponse() -> complete()
     *    complete() 内部已经 remove 了。
     *
     * 2. timeout 先发生：
     *    waitFor() 把 call 标记为 timeout。
     *    这里 remove() 负责从 map 里清理，避免泄漏。
     */
    static_cast<void>(pendingCalls_.remove(requestId));

    if (state == PendingCall::State::kDone) {
        const std::string responseBytes = call->responseBytes();

        if (!response.ParseFromString(responseBytes)) {
            LOG_WARN << "RpcChannel::callUnary failed: parse business response "
                     << "failed, service=" << service << ", method=" << method
                     << ", requestId=" << requestId
                     << ", responseBytes=" << responseBytes.size();
            return false;
        }
        LOG_INFO << "RpcChannel::callUnary success: service=" << service
                 << ", method=" << method << ", requestId=" << requestId
                 << ", responseBytes=" << responseBytes.size();
        return true;
    }
    if (state == PendingCall::State::kTimeout) {
        LOG_WARN << "RpcChannel::callUnary timeout: service=" << service
                 << ", method=" << method << ", requestId=" << requestId
                 << ", error=" << call->errorText();
        return false;
    }

    if (state == PendingCall::State::kFailed) {
        LOG_WARN << "RpcChannel::callUnary failed: service=" << service
                 << ", method=" << method << ", requestId=" << requestId
                 << ", error=" << call->errorText();
        return false;
    }

    LOG_ERROR << "RpcChannel::callUnary unexpected state: requestId="
              << requestId << ", state=" << static_cast<int>(state);
    return false;
}

void RpcChannel::onMessage(net::Buffer* buffer) {
    if (buffer == nullptr) {
        LOG_WARN << "RpcChannel::onMessage got null buffer";
        return;
    }
    while (true) {
        RpcMessage message;
        const RpcCodec::DecodeStatus status =
            codec_.tryDecode(*buffer, message);

        if (status == RpcCodec::DecodeStatus::kNeedMore) {
            break;
        }
        if (status == RpcCodec::DecodeStatus::kInvalid) {
            handleDecodeError();
            return;
        }
        if (status != RpcCodec::DecodeStatus::kOk) {
            LOG_ERROR << "RpcChannel::onMessage got unknown decode status="
                      << static_cast<int>(status);
            handleDecodeError();
            return;
        }
        if (!message.valid()) {
            LOG_WARN << "RpcChannel::onMessage got invalid RpcMessage";
            handleDecodeError();
            return;
        }
        switch (message.frameType()) {
        case FrameType::UNARY_RESPONSE:
            handleUnaryResponse(message);
            break;
        case FrameType::ERROR_FRAME:
            handleErrorFrame(message);
            break;

        case FrameType::HEARTBEAT_PING:
        case FrameType::HEARTBEAT_PONG:
            break;
        case FrameType::STREAM_OPEN:
        case FrameType::STREAM_DATA:
        case FrameType::STREAM_END:
        case FrameType::STREAM_CANCEL:
            LOG_WARN << "RpcChannel::onMessage got stream frame before "
                     << "stream client is implemented, requestId="
                     << message.requestId()
                     << ", streamId=" << message.streamId() << ", frameType="
                     << frameTypeToString(message.frameType());
            break;
        case FrameType::UNARY_REQUEST:
        case FrameType::UNKNOWN:
        default:
            LOG_WARN << "RpcChannel::onMessage got unexpected frame type: "
                     << frameTypeToString(message.frameType())
                     << ", requestId=" << message.requestId()
                     << ", streamId=" << message.streamId();
            break;
        }
    }
}
void RpcChannel::onConnectionClosed() {
    const std::size_t failed =
        pendingCalls_.failAll("connection closed before rpc response");
    if (failed > 0) {
        LOG_WARN << "RpcChannel::onConnectionClosed failed pending calls: "
                 << failed;
    }
}

std::uint64_t RpcChannel::nextRequestId() noexcept {
    return nextRequestId_.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t RpcChannel::nextStreamId() noexcept {
    return nextStreamId_.fetch_add(1, std::memory_order_relaxed);
}

bool RpcChannel::buildUnaryRequestMessage(
    const std::string& service, const std::string& method,
    const google::protobuf::Message& request, std::uint64_t requestId,
    RpcMessage* outMessage) const {
    if (outMessage == nullptr) {
        return false;
    }
    if (requestId == 0) {
        LOG_WARN << "RpcChannel::buildUnaryRequestMessage failed: "
                 << "requestId is 0";
        return false;
    }

    std::string requestPayload;
    if (!request.SerializeToString(&requestPayload)) {
        LOG_WARN << "RpcChannel failed to serialize business request, "
                 << "service=" << service << ", method=" << method
                 << ", requestId=" << requestId;
        return false;
    }

    UnaryRequestMeta requestMeta;
    requestMeta.set_service_name(service);
    requestMeta.set_method_name(method);
    requestMeta.set_request_payload(std::move(requestPayload));
    std::string rpcPayload;
    if (!requestMeta.SerializeToString(&rpcPayload)) {
        LOG_WARN << "RpcChannel failed to serialize UnaryRequestMeta, "
                 << "service=" << service << ", method=" << method
                 << ", requestId=" << requestId;
        return false;
    }
    /*
     * unary 没有逻辑流，streamId 先统一用 0。
     * 后续 streaming 使用非 0 streamId。
     */
    constexpr std::uint32_t kUnaryStreamId = 0;
    RpcMessage message(FrameType::UNARY_REQUEST, kUnaryStreamId, requestId,
                       std::move(rpcPayload));
    if (!message.valid()) {
        LOG_WARN << "RpcChannel built invalid UNARY_REQUEST, "
                 << "service=" << service << ", method=" << method
                 << ", requestId=" << requestId
                 << ", streamId=" << kUnaryStreamId
                 << ", payloadSize=" << message.payloadSize();
        return false;
    }
    *outMessage = std::move(message);
    return true;
}

bool RpcChannel::sendMessage(const RpcMessage& message) {
    if (connection_) {
        LOG_WARN << "RpcChannel::sendMessage failed: null connection";
        return false;
    }

    if (!message.valid()) {
        LOG_WARN << "RpcChannel::sendMessage got invalid message, requestId="
                 << message.requestId() << ", streamId=" << message.streamId();
        return false;
    }
    net::Buffer output;
    static_cast<void>(codec_.encode(message, output));
    connection_->send(output.retrieveAllAsString());

    LOG_INFO << "RpcChannel sent frame type="
             << frameTypeToString(message.frameType())
             << ", requestId=" << message.requestId()
             << ", streamId=" << message.streamId()
             << ", payloadSize=" << message.payloadSize();

    return true;
}

void RpcChannel::handleUnaryResponse(const RpcMessage& message) {
    if (message.requestId() == 0) {
        LOG_WARN << "RpcChannel got UNARY_RESPONSE with requestId=0";
        return;
    }

    UnaryResponseMeta responseMeta;

    if (!responseMeta.ParseFromString(message.payload())) {
        LOG_WARN << "RpcChannel failed to parse UnaryResponseMeta, requestId="
                 << message.requestId();

        static_cast<void>(pendingCalls_.fail(
            message.requestId(), "failed to parse UnaryResponseMeta"));

        return;
    }

    if (responseMeta.error_code() != RPC_OK) {
        std::string errorText = responseMeta.error_text();
        if (errorText.empty()) {
            errorText = "server returned unary rpc error";
        }
        LOG_WARN << "RpcChannel got unary error response, requestId="
                 << message.requestId() << ", errorCode="
                 << static_cast<int>(responseMeta.error_code())
                 << ", error=" << errorText;
        static_cast<void>(
            pendingCalls_.fail(message.requestId(), std::move(errorText)));

        return;
    }
    const auto result = pendingCalls_.complete(message.requestId(),
                                               responseMeta.response_payload());
    if (result == PendingCallManager::FinishResult::kNotFound) {
        LOG_WARN << "RpcChannel got late or unknown unary response, requestId="
                 << message.requestId();
        return;
    }

    if (result == PendingCallManager::FinishResult::kAlreadyFinished) {
        LOG_WARN << "RpcChannel got duplicate or already finished unary "
                 << "response, requestId=" << message.requestId();
        return;
    }
    LOG_INFO << "RpcChannel completed unary response, requestId="
             << message.requestId() << ", responsePayloadSize="
             << responseMeta.response_payload().size();
}

void RpcChannel::handleErrorFrame(const RpcMessage& message) {
    if (message.requestId() == 0) {
        LOG_WARN << "RpcChannel got ERROR_FRAME with requestId=0";
        return;
    }
    ErrorFrameMeta errorMeta;
    if (!errorMeta.ParseFromString(message.payload())) {
        LOG_WARN << "RpcChannel failed to parse ErrorFrameMeta, requestId="
                 << message.requestId();
        static_cast<void>(pendingCalls_.fail(message.requestId(),
                                             "failed to parse ErrorFrameMeta"));

        return;
    }

    std::string errorText = errorMeta.error_text();

    if (errorText.empty()) {
        errorText = "server returned ERROR_FRAME";
    }
    LOG_WARN << "RpcChannel got ERROR_FRAME, requestId=" << message.requestId()
             << ", streamId=" << message.streamId()
             << ", errorCode=" << static_cast<int>(errorMeta.error_code())
             << ", error=" << errorText;
    static_cast<void>(
        pendingCalls_.fail(message.requestId(), std::move(errorText)));
}

void RpcChannel::handleDecodeError() {
    LOG_WARN << "RpcChannel decode error, fail all pending calls";
    static_cast<void>(pendingCalls_.failAll("rpc codec decode error"));

    if (connection_) {
        connection_->shutdown();
    }
}

std::uint32_t RpcChannel::openStream(const std::string& service,
                                     const std::string& method,
                                     const google::protobuf::Message& request) {
    (void)service;
    (void)method;
    (void)request;

    LOG_WARN << "RpcChannel::openStream is not implemented in Step 14";
    return 0;
}

bool RpcChannel::sendStreamData(std::uint32_t streamId,
                                const std::string& chunk) {
    (void)streamId;
    (void)chunk;

    LOG_WARN << "RpcChannel::sendStreamData is not implemented in Step 14";
    return false;
}

bool RpcChannel::sendStreamEnd(std::uint32_t streamId) {
    (void)streamId;

    LOG_WARN << "RpcChannel::sendStreamEnd is not implemented in Step 14";
    return false;
}

bool RpcChannel::cancelStream(std::uint32_t streamId) {
    (void)streamId;

    LOG_WARN << "RpcChannel::cancelStream is not implemented in Step 14";
    return false;
}

} // namespace novanet::rpc