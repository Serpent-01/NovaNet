#include "novanet/rpc/core/RpcDispatcher.h"

#include <string>
#include <utility>
#include <vector>

#include "novanet/base/Logger.h"
#include "novanet/rpc/protocol/FrameType.h"

namespace novanet::rpc {

RpcDispatcher::RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker)
    : registry_(registry), invoker_(invoker) {
}

bool RpcDispatcher::dispatch(const RpcMessage& msg,
                             std::vector<RpcMessage>& outResponses) const {
    outResponses.clear();

    if (!msg.valid()) {
        LOG_WARN << "RpcDispatcher got invalid RpcMessage";

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
        LOG_WARN << "RpcDispatcher failed to parse UnaryRequestMeta, requestId="
                 << msg.requestId();

        return appendUnaryErrorResponse(msg, RPC_PARSE_REQUEST_FAILED,
                                        "failed to parse UnaryRequestMeta",
                                        outResponses);
    }

    LOG_INFO << "RpcDispatcher unary request: service="
             << requestMeta.service_name()
             << ", method=" << requestMeta.method_name()
             << ", requestId=" << msg.requestId()
             << ", streamId=" << msg.streamId()
             << ", requestPayloadSize=" << requestMeta.request_payload().size();

    if (requestMeta.service_name().empty()) {
        LOG_WARN << "RpcDispatcher bad request: empty service_name, requestId="
                 << msg.requestId();

        return appendUnaryErrorResponse(
            msg, RPC_BAD_REQUEST, "UnaryRequestMeta.service_name is empty",
            outResponses);
    }

    if (requestMeta.method_name().empty()) {
        LOG_WARN << "RpcDispatcher bad request: empty method_name, requestId="
                 << msg.requestId();

        return appendUnaryErrorResponse(msg, RPC_BAD_REQUEST,
                                        "UnaryRequestMeta.method_name is empty",
                                        outResponses);
    }

    const auto* serviceMeta = registry_.findService(requestMeta.service_name());
    if (serviceMeta == nullptr) {
        LOG_WARN << "RpcDispatcher service not found: "
                 << requestMeta.service_name()
                 << ", requestId=" << msg.requestId();

        return appendUnaryErrorResponse(
            msg, RPC_SERVICE_NOT_FOUND,
            "service not found: " + requestMeta.service_name(), outResponses);
    }

    const auto* methodMeta =
        registry_.findMethod(*serviceMeta, requestMeta.method_name());

    if (methodMeta == nullptr) {
        LOG_WARN << "RpcDispatcher method not found: "
                 << requestMeta.service_name() << "."
                 << requestMeta.method_name()
                 << ", requestId=" << msg.requestId();

        return appendUnaryErrorResponse(
            msg, RPC_METHOD_NOT_FOUND,
            "method not found: " + requestMeta.service_name() + "." +
                requestMeta.method_name(),
            outResponses);
    }

    auto invokeResult = invoker_.invokeUnary(*serviceMeta, *methodMeta,
                                             requestMeta.request_payload());

    if (invokeResult.failed()) {
        LOG_WARN << "RpcDispatcher invoke failed: code="
                 << static_cast<int>(invokeResult.errorCode())
                 << ", error=" << invokeResult.errorText()
                 << ", requestId=" << msg.requestId();

        return appendUnaryErrorResponse(msg, invokeResult.errorCode(),
                                        invokeResult.errorText(), outResponses);
    }

    LOG_INFO << "RpcDispatcher invoke success, requestId=" << msg.requestId()
             << ", responseBytes=" << invokeResult.responseBytes().size();

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
        LOG_ERROR << "RpcDispatcher failed to serialize UnaryResponseMeta ok, "
                  << "requestId=" << requestMsg.requestId();
        return false;
    }

    RpcMessage response(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                        requestMsg.requestId(), std::move(payload));

    if (!response.valid()) {
        LOG_ERROR << "RpcDispatcher generated invalid UNARY_RESPONSE, "
                  << "requestId=" << response.requestId()
                  << ", streamId=" << response.streamId()
                  << ", type=" << response.type()
                  << ", totalLen=" << response.totalLen()
                  << ", payloadSize=" << response.payloadSize();
        return false;
    }

    outResponses.emplace_back(std::move(response));
    return true;
}

bool RpcDispatcher::appendUnaryErrorResponse(
    const RpcMessage& requestMsg, RpcErrorCode errorCode, std::string errorText,
    std::vector<RpcMessage>& outResponses) const {
    UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(errorCode);
    responseMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!responseMeta.SerializeToString(&payload)) {
        LOG_ERROR
            << "RpcDispatcher failed to serialize UnaryResponseMeta error, "
            << "requestId=" << requestMsg.requestId()
            << ", errorCode=" << static_cast<int>(errorCode);
        return false;
    }

    RpcMessage response(FrameType::UNARY_RESPONSE, requestMsg.streamId(),
                        requestMsg.requestId(), std::move(payload));

    if (!response.valid()) {
        LOG_ERROR << "RpcDispatcher generated invalid unary error response, "
                  << "requestId=" << response.requestId()
                  << ", streamId=" << response.streamId()
                  << ", type=" << response.type()
                  << ", totalLen=" << response.totalLen()
                  << ", payloadSize=" << response.payloadSize()
                  << ", errorCode=" << static_cast<int>(errorCode);
        return false;
    }

    outResponses.emplace_back(std::move(response));
    return true;
}

bool RpcDispatcher::appendErrorFrame(
    const RpcMessage& requestMsg, RpcErrorCode errorCode, std::string errorText,
    std::vector<RpcMessage>& outResponses) const {
    ErrorFrameMeta errorMeta;
    errorMeta.set_error_code(errorCode);
    errorMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!errorMeta.SerializeToString(&payload)) {
        LOG_ERROR << "RpcDispatcher failed to serialize ErrorFrameMeta, "
                  << "requestId=" << requestMsg.requestId()
                  << ", errorCode=" << static_cast<int>(errorCode);
        return false;
    }

    RpcMessage response(FrameType::ERROR_FRAME, requestMsg.streamId(),
                        requestMsg.requestId(), std::move(payload));

    if (!response.valid()) {
        LOG_ERROR << "RpcDispatcher generated invalid ERROR_FRAME, "
                  << "requestId=" << response.requestId()
                  << ", streamId=" << response.streamId()
                  << ", type=" << response.type()
                  << ", totalLen=" << response.totalLen()
                  << ", payloadSize=" << response.payloadSize()
                  << ", errorCode=" << static_cast<int>(errorCode);
        return false;
    }

    outResponses.emplace_back(std::move(response));
    return true;
}

}  // namespace novanet::rpc