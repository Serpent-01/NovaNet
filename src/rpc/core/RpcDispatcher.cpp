#include "novanet/rpc/core/RpcDispatcher.h"

#include <string>
#include <utility>

#include "novanet/rpc/protocol/FrameType.h"

namespace novanet::rpc {

RpcDispatcher::RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker)
    : registry_(registry), invoker_(invoker) {
}

bool RpcDispatcher::dispatch(const RpcMessage& msg,
                             std::vector<RpcMessage>& outResponses) const {
    outResponses.clear();

    if (!msg.valid()) {
        return appendErrorFrame(msg, RPC_INVALID_FRAME, "invalid rpc message",
                                outResponses);
    }

    switch (msg.frameType()) {
        case FrameType::UNARY_REQUEST:
            return dispatchUnaryRequest(msg, outResponses);

        case FrameType::UNARY_RESPONSE:
        case FrameType::STREAM_OPEN:
        case FrameType::STREAM_DATA:
        case FrameType::STREAM_END:
        case FrameType::STREAM_CANCEL:
        case FrameType::HEARTBEAT_PING:
        case FrameType::HEARTBEAT_PONG:
        case FrameType::ERROR_FRAME:
        case FrameType::UNKNOWN:
        default:
            return appendErrorFrame(
                msg, RPC_UNSUPPORTED_FRAME_TYPE,
                "unsupported frame type in unary dispatcher: " +
                    std::string(frameTypeToString(msg.frameType())),
                outResponses);
    }
}

bool RpcDispatcher::dispatchUnaryRequest(
    const RpcMessage& msg, std::vector<RpcMessage>& outResponses) const {
    UnaryRequestMeta requestMeta;
    if (!requestMeta.ParseFromString(msg.payload())) {
        return appendUnaryErrorResponse(msg, RPC_PARSE_REQUEST_FAILED,
                                        "failed to parse UnaryRequestMeta",
                                        outResponses);
    }

    if (requestMeta.service_name().empty()) {
        return appendUnaryErrorResponse(
            msg, RPC_BAD_REQUEST, "UnaryRequestMeta.service_name is empty",
            outResponses);
    }

    if (requestMeta.method_name().empty()) {
        return appendUnaryErrorResponse(msg, RPC_BAD_REQUEST,
                                        "UnaryRequestMeta.method_name is empty",
                                        outResponses);
    }

    const auto* serviceMeta = registry_.findService(requestMeta.service_name());
    if (serviceMeta == nullptr) {
        return appendUnaryErrorResponse(
            msg, RPC_SERVICE_NOT_FOUND,
            "service not found: " + requestMeta.service_name(), outResponses);
    }

    const auto* methodMeta =
        registry_.findMethod(*serviceMeta, requestMeta.method_name());

    if (methodMeta == nullptr) {
        return appendUnaryErrorResponse(
            msg, RPC_METHOD_NOT_FOUND,
            "method not found: " + requestMeta.service_name() + "." +
                requestMeta.method_name(),
            outResponses);
    }

    auto invokeResult = invoker_.invokeUnary(*serviceMeta, *methodMeta,
                                             requestMeta.request_payload());

    if (invokeResult.failed()) {
        return appendUnaryErrorResponse(msg, invokeResult.errorCode(),
                                        invokeResult.errorText(), outResponses);
    }

    return appendUnaryOkResponse(msg, invokeResult.releaseResponseBytes(),
                                 outResponses);
}

bool RpcDispatcher::appendUnaryOkResponse(
    const RpcMessage& requestMsg, std::string responsePayload,
    std::vector<RpcMessage>& outResponses) const {
    UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(RPC_OK);
    responseMeta.set_response_payload(std::move(responsePayload));

    std::string payload;
    if (!responseMeta.SerializeToString(&payload)) {
        return false;
    }

    outResponses.emplace_back(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                              requestMsg.requestId(), std::move(payload));

    return outResponses.back().valid();
}

bool RpcDispatcher::appendUnaryErrorResponse(
    const RpcMessage& requestMsg, RpcErrorCode errorCode, std::string errorText,
    std::vector<RpcMessage>& outResponses) const {
    UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(errorCode);
    responseMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!responseMeta.SerializeToString(&payload)) {
        return false;
    }

    outResponses.emplace_back(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                              requestMsg.requestId(), std::move(payload));

    return outResponses.back().valid();
}

bool RpcDispatcher::appendErrorFrame(
    const RpcMessage& requestMsg, RpcErrorCode errorCode, std::string errorText,
    std::vector<RpcMessage>& outResponses) const {
    ErrorFrameMeta errorMeta;
    errorMeta.set_error_code(errorCode);
    errorMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!errorMeta.SerializeToString(&payload)) {
        return false;
    }

    outResponses.emplace_back(FrameType::ERROR_FRAME, requestMsg.streamId(),
                              requestMsg.requestId(), std::move(payload));

    return outResponses.back().valid();
}

}  // namespace novanet::rpc