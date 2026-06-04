#include "novanet/rpc/core/RpcDispatcher.h"

#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/stream/StreamFrame.h"

namespace novanet::rpc {

RpcDispatcher::RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker,
                             StreamMethodInvoker& streamInvoker)
    : registry_(registry), invoker_(invoker), streamInvoker_(streamInvoker) {
}

bool RpcDispatcher::dispatch(const RpcMessage& msg, StreamManager& streamManager,
                             std::vector<RpcMessage>& immediateResponses,
                             const std::shared_ptr<StreamResponder>& responder) {
    switch (msg.frameType()) {
        case FrameType::UNARY_REQUEST:
            return dispatchUnaryRequest(msg, immediateResponses);

        case FrameType::STREAM_OPEN:
            return dispatchStreamOpen(msg, streamManager, immediateResponses, responder);

        case FrameType::STREAM_DATA:
            return dispatchStreamData(msg, streamManager, immediateResponses);

        case FrameType::STREAM_END:
            return dispatchStreamEnd(msg, streamManager, immediateResponses);

        case FrameType::STREAM_CANCEL:
            return dispatchStreamCancel(msg, streamManager, immediateResponses);

        case FrameType::HEARTBEAT_PING:
            return dispatchHeartbeatPing(msg, immediateResponses);

        case FrameType::HEARTBEAT_PONG:
            return dispatchHeartbeatPong(msg, immediateResponses);

        case FrameType::ERROR_FRAME:
            LOG_WARN << "[RpcDispatcher] received ERROR_FRAME from peer, requestId="
                     << msg.requestId() << ", streamId=" << msg.streamId();
            return true;

        case FrameType::UNARY_RESPONSE:
        case FrameType::UNKNOWN:
        default:
            return appendErrorFrame(msg, meta::RPC_UNSUPPORTED_FRAME_TYPE,
                                    "unsupported frame type for server dispatcher",
                                    immediateResponses);
    }
}

bool RpcDispatcher::dispatchUnaryRequest(const RpcMessage& msg,
                                         std::vector<RpcMessage>& immediateResponses) {
    if (msg.requestId() == 0) {
        return appendErrorFrame(msg, meta::RPC_BAD_REQUEST,
                                "UNARY_REQUEST requires non-zero requestId",
                                immediateResponses);
    }

    meta::UnaryRequestMeta requestMeta;
    if (!requestMeta.ParseFromString(msg.payload())) {
        LOG_WARN << "[RpcDispatcher] parse UnaryRequestMeta failed, requestId="
                 << msg.requestId();
        return appendUnaryErrorResponse(msg, meta::RPC_PARSE_REQUEST_FAILED,
                                        "failed to parse UnaryRequestMeta",
                                        immediateResponses);
    }

    if (requestMeta.service_name().empty() || requestMeta.method_name().empty()) {
        return appendUnaryErrorResponse(msg, meta::RPC_BAD_REQUEST,
                                        "unary service_name or method_name is empty",
                                        immediateResponses);
    }

    const auto* serviceMeta = registry_.findService(requestMeta.service_name());
    if (serviceMeta == nullptr) {
        return appendUnaryErrorResponse(
            msg, meta::RPC_SERVICE_NOT_FOUND,
            "service not found: " + requestMeta.service_name(), immediateResponses);
    }

    const auto* methodMeta =
        registry_.findMethod(*serviceMeta, requestMeta.method_name());
    if (methodMeta == nullptr) {
        return appendUnaryErrorResponse(
            msg, meta::RPC_METHOD_NOT_FOUND,
            "method not found: " + requestMeta.service_name() + "." +
                requestMeta.method_name(),
            immediateResponses);
    }

    auto invokeResult =
        invoker_.invokeUnary(*serviceMeta, *methodMeta, requestMeta.request_payload());

    if (invokeResult.failed()) {
        return appendUnaryErrorResponse(msg, invokeResult.errorCode(),
                                        invokeResult.errorText(), immediateResponses);
    }

    return appendUnaryOkResponse(msg, invokeResult.releaseResponseBytes(),
                                 immediateResponses);
}

bool RpcDispatcher::dispatchStreamOpen(
    const RpcMessage& msg, StreamManager& streamManager,
    std::vector<RpcMessage>& immediateResponses,
    const std::shared_ptr<StreamResponder>& responder) {
    if (!responder) {
        return appendErrorFrame(msg, meta::RPC_INTERNAL_ERROR, "StreamResponder is null",
                                immediateResponses);
    }

    if (msg.streamId() == 0 || msg.requestId() == 0) {
        return appendErrorFrame(msg, meta::RPC_BAD_REQUEST,
                                "STREAM_OPEN requires non-zero streamId and requestId",
                                immediateResponses);
    }

    StreamFrame frame(msg);
    if (!frame.valid() || !frame.isOpen()) {
        return appendErrorFrame(msg, meta::RPC_INVALID_FRAME, "invalid STREAM_OPEN frame",
                                immediateResponses);
    }

    meta::StreamOpenMeta openMeta;
    if (!openMeta.ParseFromString(frame.payload())) {
        return appendErrorFrame(msg, meta::RPC_PARSE_REQUEST_FAILED,
                                "failed to parse StreamOpenMeta", immediateResponses);
    }

    const RpcStatus validation =
        streamInvoker_.validate(openMeta.service_name(), openMeta.method_name());
    if (validation.failed()) {
        return appendErrorFrame(msg, validation.errorCode(),
                                validation.errorText(), immediateResponses);
    }

    auto session =
        streamManager.createStream(frame.streamId(), frame.requestId(),
                                   openMeta.service_name(), openMeta.method_name());
    if (!session) {
        return appendErrorFrame(msg, meta::RPC_STREAM_PROTOCOL_ERROR,
                                "failed to create stream session", immediateResponses);
    }

    if (!session->markRemoteEnd()) {
        (void)streamManager.removeStream(frame.streamId());
        return appendErrorFrame(msg, meta::RPC_STREAM_PROTOCOL_ERROR,
                                "failed to mark remote end after STREAM_OPEN",
                                immediateResponses);
    }

    RpcStatus startStatus = streamInvoker_.start(
        frame.streamId(), frame.requestId(), openMeta.service_name(),
        openMeta.method_name(), openMeta.request_payload(), responder);
    if (startStatus.failed()) {
        (void)session->markCancelled(startStatus.errorText());
        (void)streamManager.removeStream(frame.streamId());

        return appendErrorFrame(msg, startStatus.errorCode(),
                                startStatus.errorText(), immediateResponses);
    }

    LOG_INFO << "[RpcDispatcher] stream opened, streamId=" << frame.streamId()
             << ", requestId=" << frame.requestId()
             << ", service=" << openMeta.service_name()
             << ", method=" << openMeta.method_name();

    return true;
}

bool RpcDispatcher::dispatchStreamData(const RpcMessage& msg,
                                       StreamManager& streamManager,
                                       std::vector<RpcMessage>& immediateResponses) {
    const auto result = streamManager.handleDataFrame(msg);
    if (result == StreamManager::Result::kOk) {
        return true;
    }

    return appendErrorFrame(msg, streamResultToErrorCode(result),
                            "failed to handle STREAM_DATA: " +
                                std::string(StreamManager::resultToString(result)),
                            immediateResponses);
}

bool RpcDispatcher::dispatchStreamEnd(const RpcMessage& msg, StreamManager& streamManager,
                                      std::vector<RpcMessage>& immediateResponses) {
    const auto result = streamManager.handleEndFrame(msg);
    if (result == StreamManager::Result::kOk) {
        return true;
    }

    return appendErrorFrame(msg, streamResultToErrorCode(result),
                            "failed to handle STREAM_END: " +
                                std::string(StreamManager::resultToString(result)),
                            immediateResponses);
}

bool RpcDispatcher::dispatchStreamCancel(const RpcMessage& msg,
                                         StreamManager& streamManager,
                                         std::vector<RpcMessage>& immediateResponses) {
    if (msg.streamId() == 0 || msg.requestId() == 0) {
        return appendErrorFrame(msg, meta::RPC_BAD_REQUEST,
                                "STREAM_CANCEL requires non-zero streamId/requestId",
                                immediateResponses);
    }

    const auto result = streamManager.handleCancelFrame(msg);
    if (result == StreamManager::Result::kOk) {
        LOG_INFO << "[RpcDispatcher] stream cancelled, streamId=" << msg.streamId();
        return true;
    }

    return appendErrorFrame(msg, streamResultToErrorCode(result),
                            "failed to handle STREAM_CANCEL: " +
                                std::string(StreamManager::resultToString(result)),
                            immediateResponses);
}

bool RpcDispatcher::dispatchHeartbeatPing(const RpcMessage& msg,
                                          std::vector<RpcMessage>& immediateResponses) {
    return appendHeartbeatPong(msg, immediateResponses);
}

bool RpcDispatcher::dispatchHeartbeatPong(const RpcMessage& msg,
                                          std::vector<RpcMessage>& immediateResponses) {
    (void)msg;
    (void)immediateResponses;
    return true;
}

bool RpcDispatcher::appendUnaryOkResponse(
    const RpcMessage& requestMsg, std::string responsePayload,
    std::vector<RpcMessage>& immediateResponses) const {
    meta::UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(meta::RPC_OK);
    responseMeta.set_response_payload(std::move(responsePayload));

    std::string payload;
    if (!responseMeta.SerializeToString(&payload)) {
        return false;
    }

    RpcMessage response(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                        requestMsg.requestId(), std::move(payload));

    if (!response.valid()) {
        return false;
    }

    immediateResponses.emplace_back(std::move(response));
    return true;
}

bool RpcDispatcher::appendUnaryErrorResponse(
    const RpcMessage& requestMsg, meta::RpcErrorCode errorCode, std::string errorText,
    std::vector<RpcMessage>& immediateResponses) const {
    meta::UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(errorCode);
    responseMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!responseMeta.SerializeToString(&payload)) {
        return false;
    }

    RpcMessage response(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                        requestMsg.requestId(), std::move(payload));

    if (!response.valid()) {
        return false;
    }

    immediateResponses.emplace_back(std::move(response));
    return true;
}

bool RpcDispatcher::appendHeartbeatPong(
    const RpcMessage& requestMsg, std::vector<RpcMessage>& immediateResponses) const {
    RpcMessage pong(FrameType::HEARTBEAT_PONG, 0, requestMsg.requestId(),
                    requestMsg.payload());

    if (!pong.valid()) {
        return false;
    }

    immediateResponses.emplace_back(std::move(pong));
    return true;
}

bool RpcDispatcher::appendErrorFrame(const RpcMessage& requestMsg,
                                     meta::RpcErrorCode errorCode, std::string errorText,
                                     std::vector<RpcMessage>& immediateResponses) const {
    meta::ErrorFrameMeta errorMeta;
    errorMeta.set_error_code(errorCode);
    errorMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!errorMeta.SerializeToString(&payload)) {
        return false;
    }

    RpcMessage errorFrame(FrameType::ERROR_FRAME, requestMsg.streamId(),
                          requestMsg.requestId(), std::move(payload));

    if (!errorFrame.valid()) {
        return false;
    }

    immediateResponses.emplace_back(std::move(errorFrame));
    return true;
}

meta::RpcErrorCode RpcDispatcher::streamResultToErrorCode(
    StreamManager::Result result) noexcept {
    switch (result) {
        case StreamManager::Result::kOk:
            return meta::RPC_OK;
        case StreamManager::Result::kInvalidArgument:
            return meta::RPC_BAD_REQUEST;
        case StreamManager::Result::kInvalidFrame:
            return meta::RPC_INVALID_FRAME;
        case StreamManager::Result::kDuplicateStream:
            return meta::RPC_STREAM_PROTOCOL_ERROR;
        case StreamManager::Result::kStreamNotFound:
            return meta::RPC_STREAM_NOT_FOUND;
        case StreamManager::Result::kStateRejected:
            return meta::RPC_STREAM_PROTOCOL_ERROR;
        default:
            return meta::RPC_UNKNOWN_ERROR;
    }
}

}  // namespace novanet::rpc
