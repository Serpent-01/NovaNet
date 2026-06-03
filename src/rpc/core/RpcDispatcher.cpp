#include "novanet/rpc/core/RpcDispatcher.h"

#include <utility>

#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/stream/StreamFrame.h"

namespace novanet::rpc {

namespace chat = ::novanet::ai::chat;

namespace {

constexpr const char* kChatServiceFullName = "novanet.ai.chat.ChatService";
constexpr const char* kGenerateMethodName = "Generate";

}  // namespace

RpcDispatcher::RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker,
                             AiProvider& aiProvider, AiExecutor& aiExecutor)
    : registry_(registry),
      invoker_(invoker),
      aiProvider_(aiProvider),
      aiExecutor_(aiExecutor) {
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
            /*
             * 服务端收到 ERROR_FRAME 通常只记录日志。
             * RpcDispatcher 不负责日志策略，这里不回包，
             * 避免 ERROR_FRAME 互相打架。
             */
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
        return appendUnaryErrorResponse(msg, meta::RPC_PARSE_REQUEST_FAILED,
                                        "failed to parse UnaryRequestMeta",
                                        immediateResponses);
    }

    if (requestMeta.service_name().empty()) {
        return appendUnaryErrorResponse(msg, meta::RPC_BAD_REQUEST,
                                        "UnaryRequestMeta.service_name is empty",
                                        immediateResponses);
    }

    if (requestMeta.method_name().empty()) {
        return appendUnaryErrorResponse(msg, meta::RPC_BAD_REQUEST,
                                        "UnaryRequestMeta.method_name is empty",
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
        return appendErrorFrame(msg, meta::RPC_INTERNAL_ERROR,
                                "StreamResponder is not configured", immediateResponses);
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

    if (openMeta.service_name().empty()) {
        return appendErrorFrame(msg, meta::RPC_BAD_REQUEST,
                                "StreamOpenMeta.service_name is empty",
                                immediateResponses);
    }

    if (openMeta.method_name().empty()) {
        return appendErrorFrame(msg, meta::RPC_BAD_REQUEST,
                                "StreamOpenMeta.method_name is empty",
                                immediateResponses);
    }

    meta::RpcErrorCode methodErrorCode = meta::RPC_OK;
    std::string methodErrorText;

    if (!validateRegisteredMethod(openMeta.service_name(), openMeta.method_name(),
                                  methodErrorCode, methodErrorText)) {
        return appendErrorFrame(msg, methodErrorCode, std::move(methodErrorText),
                                immediateResponses);
    }

    if (!isSupportedStreamingMethod(openMeta.service_name(), openMeta.method_name(),
                                    methodErrorCode, methodErrorText)) {
        return appendErrorFrame(msg, methodErrorCode, std::move(methodErrorText),
                                immediateResponses);
    }

    chat::GenerateRequest request;
    if (!request.ParseFromString(openMeta.request_payload())) {
        return appendErrorFrame(msg, meta::RPC_PARSE_REQUEST_FAILED,
                                "failed to parse chat.GenerateRequest",
                                immediateResponses);
    }

    auto session =
        streamManager.createStream(frame.streamId(), frame.requestId(),
                                   openMeta.service_name(), openMeta.method_name());

    if (!session) {
        return appendErrorFrame(
            msg, meta::RPC_STREAM_PROTOCOL_ERROR,
            "failed to create stream session, duplicate or invalid stream",
            immediateResponses);
    }

    /*
     * server streaming 语义：
     * STREAM_OPEN 已经携带完整请求。
     * 服务端视角下，对端请求方向已经结束。
     */
    if (!session->markRemoteEnd()) {
        (void)streamManager.removeStream(frame.streamId());

        return appendErrorFrame(msg, meta::RPC_STREAM_PROTOCOL_ERROR,
                                "failed to mark remote end after STREAM_OPEN",
                                immediateResponses);
    }

    /*
     * 核心要求：
     * 不在 EventLoop 线程里调用 aiProvider.generateStreaming()。
     * 这里只提交任务，dispatch 立即返回。
     */
    meta::RpcErrorCode submitErrorCode = meta::RPC_OK;
    std::string submitErrorText;

    if (!submitGenerateTask(frame.streamId(), frame.requestId(), std::move(request),
                            responder, submitErrorCode, submitErrorText)) {
        (void)session->markCancelled(submitErrorText);
        (void)streamManager.removeStream(frame.streamId());

        return appendErrorFrame(msg, submitErrorCode, std::move(submitErrorText),
                                immediateResponses);
    }

    /*
     * STREAM_OPEN 成功后不立即回包。
     * 后续 STREAM_DATA / STREAM_END 由 AiExecutor worker + StreamResponder 异步发送。
     */
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
                                "STREAM_CANCEL requires non-zero streamId and requestId",
                                immediateResponses);
    }

    StreamFrame frame(msg);
    if (!frame.valid() || !frame.isCancel()) {
        return appendErrorFrame(msg, meta::RPC_INVALID_FRAME,
                                "invalid STREAM_CANCEL frame", immediateResponses);
    }

    /*
     * cancel 后，StreamManager 更新状态。
     * AiExecutor worker 侧 responder->shouldStop(streamId)
     * 会尽快返回 cancelled/backpressure/closed，停止 provider 生成。
     */
    const auto result = streamManager.handleCancelFrame(msg);
    if (result == StreamManager::Result::kOk) {
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

    /*
     * PONG 通常由 RpcServer/RpcChannel 的连接上下文记录 lastPongTime。
     * RpcDispatcher 不保存连接级状态，所以这里不生成响应。
     */
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

bool RpcDispatcher::validateRegisteredMethod(const std::string& serviceName,
                                             const std::string& methodName,
                                             meta::RpcErrorCode& errorCode,
                                             std::string& errorText) const {
    const auto* serviceMeta = registry_.findService(serviceName);
    if (serviceMeta == nullptr) {
        errorCode = meta::RPC_SERVICE_NOT_FOUND;
        errorText = "service not found: " + serviceName;
        return false;
    }

    const auto* methodMeta = registry_.findMethod(*serviceMeta, methodName);
    if (methodMeta == nullptr) {
        errorCode = meta::RPC_METHOD_NOT_FOUND;
        errorText = "method not found: " + serviceName + "." + methodName;
        return false;
    }

    errorCode = meta::RPC_OK;
    errorText.clear();
    return true;
}

bool RpcDispatcher::isSupportedStreamingMethod(const std::string& serviceName,
                                               const std::string& methodName,
                                               meta::RpcErrorCode& errorCode,
                                               std::string& errorText) const {
    /*
     * 当前 Phase 4 内置 ChatService.Generate 作为 server streaming 示例。
     *
     * 注意：
     * 这里仍然是 StreamMethodInvoker 的扩展点。
     * 未来如果要支持多个 streaming service/method，
     * 应新增 StreamMethodInvoker，而不是让 RpcDispatcher 写更多 if/else。
     */
    if (serviceName == kChatServiceFullName && methodName == kGenerateMethodName) {
        errorCode = meta::RPC_OK;
        errorText.clear();
        return true;
    }

    errorCode = meta::RPC_METHOD_NOT_FOUND;
    errorText = "unsupported streaming method: " + serviceName + "." + methodName;
    return false;
}

bool RpcDispatcher::submitGenerateTask(std::uint32_t streamId, std::uint64_t requestId,
                                       chat::GenerateRequest request,
                                       std::shared_ptr<StreamResponder> responder,
                                       meta::RpcErrorCode& errorCode,
                                       std::string& errorText) {
    if (!responder) {
        errorCode = meta::RPC_INTERNAL_ERROR;
        errorText = "StreamResponder is null";
        return false;
    }

    /*
     * 不捕获 this。
     *
     * 生命周期约束：
     * RpcServer::stop() 必须先停止 AiExecutor，
     * 再销毁 AiProvider。
     */
    AiProvider* provider = &aiProvider_;

    const AiExecutor::SubmitResult submitResult =
        aiExecutor_.submit([provider, responder = std::move(responder), streamId,
                            requestId, request = std::move(request)]() mutable {
            RpcDispatcher::runGenerateTask(*provider, std::move(responder), streamId,
                                           requestId, std::move(request));
        });

    if (submitResult == AiExecutor::SubmitResult::kOk) {
        errorCode = meta::RPC_OK;
        errorText.clear();
        return true;
    }

    errorCode = executorSubmitResultToErrorCode(submitResult);
    errorText = "failed to submit AI generate task: " +
                std::string(AiExecutor::toString(submitResult));
    return false;
}

void RpcDispatcher::runGenerateTask(AiProvider& aiProvider,
                                    std::shared_ptr<StreamResponder> responder,
                                    std::uint32_t streamId, std::uint64_t requestId,
                                    chat::GenerateRequest request) {
    if (!responder) {
        return;
    }

    auto onChunk = [responder, streamId,
                    requestId](const chat::GenerateChunk& chunk) -> AiProvider::Status {
        return responder->sendData(streamId, requestId, chunk);
    };

    auto shouldStop = [responder, streamId]() -> AiProvider::Status {
        return responder->shouldStop(streamId);
    };

    const AiProvider::Status status =
        aiProvider.generateStreaming(request, std::move(onChunk), std::move(shouldStop));

    if (status.ok()) {
        /*
         * 正常完成：发送 STREAM_END(RPC_OK)。
         */
        (void)responder->sendEnd(streamId, requestId, meta::RPC_OK, "");
        return;
    }

    /*
     * 客户端主动 cancel / stream closed / connection closed：
     * 通常不再发 END，避免对已经终止的 stream 继续写。
     */
    if (status.code == AiProvider::StatusCode::kCancelled ||
        status.code == AiProvider::StatusCode::kConsumerStopped) {
        return;
    }

    /*
     * provider error / timeout / backpressure：
     * 尽量用 STREAM_END(error) 通知客户端。
     */
    const meta::RpcErrorCode code = aiStatusToErrorCode(status.code);
    const std::string text =
        defaultErrorText(status.errorText, "AI generate task failed");

    (void)responder->sendEnd(streamId, requestId, code, text);
}

meta::RpcErrorCode RpcDispatcher::aiStatusToErrorCode(
    AiProvider::StatusCode code) noexcept {
    switch (code) {
        case AiProvider::StatusCode::kOk:
            return meta::RPC_OK;

        case AiProvider::StatusCode::kInvalidRequest:
            return meta::RPC_BAD_REQUEST;

        case AiProvider::StatusCode::kCancelled:
            return meta::RPC_CANCELLED;

        case AiProvider::StatusCode::kTimeout:
            return meta::RPC_TIMEOUT;

        case AiProvider::StatusCode::kBackpressure:
            return meta::RPC_BACKPRESSURE;

        case AiProvider::StatusCode::kConsumerStopped:
            return meta::RPC_STREAM_CLOSED;

        case AiProvider::StatusCode::kProviderError:
            return meta::RPC_INVOKE_FAILED;

        default:
            return meta::RPC_UNKNOWN_ERROR;
    }
}

meta::RpcErrorCode RpcDispatcher::executorSubmitResultToErrorCode(
    AiExecutor::SubmitResult result) noexcept {
    switch (result) {
        case AiExecutor::SubmitResult::kOk:
            return meta::RPC_OK;

        case AiExecutor::SubmitResult::kInvalidTask:
            return meta::RPC_INTERNAL_ERROR;

        case AiExecutor::SubmitResult::kNotRunning:
            return meta::RPC_INTERNAL_ERROR;

        case AiExecutor::SubmitResult::kQueueFull:
            return meta::RPC_RESOURCE_EXHAUSTED;

        default:
            return meta::RPC_UNKNOWN_ERROR;
    }
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

std::string RpcDispatcher::defaultErrorText(const std::string& text,
                                            const char* fallback) {
    if (!text.empty()) {
        return text;
    }

    if (fallback != nullptr) {
        return fallback;
    }

    return "unknown error";
}

}  // namespace novanet::rpc