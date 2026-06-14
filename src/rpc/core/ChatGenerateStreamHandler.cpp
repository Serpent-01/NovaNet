#include "novanet/rpc/core/ChatGenerateStreamHandler.h"

#include <utility>

#include "novanet/base/Logger.h"

namespace novanet::rpc {

namespace chat = ::novanet::ai::chat;

namespace {
constexpr const char* kChatServiceFullName = "novanet.ai.chat.ChatService";
constexpr const char* kGenerateMethodName = "Generate";
}  // namespace

ChatGenerateStreamHandler::ChatGenerateStreamHandler(AiProvider& aiProvider,
                                                     AiExecutor& aiExecutor)
    : aiProvider_(aiProvider), aiExecutor_(aiExecutor) {
}

std::string ChatGenerateStreamHandler::serviceName() const {
    return serviceFullName();
}

std::string ChatGenerateStreamHandler::methodName() const {
    return generateMethodName();
}

RpcStatus ChatGenerateStreamHandler::start(StartContext context) {
    if (context.streamId == 0 || context.requestId == 0) {
        return RpcStatus::failure(meta::RPC_BAD_REQUEST,
                                  "streamId/requestId must be non-zero");
    }

    if (!context.responder) {
        return RpcStatus::failure(meta::RPC_INTERNAL_ERROR,
                                  "StreamResponder is null");
    }

    chat::GenerateRequest request;
    if (!request.ParseFromString(context.requestPayload)) {
        return RpcStatus::failure(meta::RPC_PARSE_REQUEST_FAILED,
                                  "failed to parse chat.GenerateRequest");
    }

    AiProvider* provider = &aiProvider_;

    const AiExecutor::SubmitResult submitResult = aiExecutor_.submit(
        [provider, responder = std::move(context.responder),
         streamId = context.streamId, requestId = context.requestId,
         request = std::move(request)]() mutable {
            ChatGenerateStreamHandler::runGenerateTask(
                *provider, std::move(responder), streamId, requestId,
                std::move(request));
        });

    if (submitResult == AiExecutor::SubmitResult::kOk) {
        return RpcStatus::success();
    }

    return RpcStatus::failure(
        executorSubmitResultToErrorCode(submitResult),
        "failed to submit AI generate task: " +
            std::string(AiExecutor::toString(submitResult)));
}

const char* ChatGenerateStreamHandler::serviceFullName() noexcept {
    return kChatServiceFullName;
}

const char* ChatGenerateStreamHandler::generateMethodName() noexcept {
    return kGenerateMethodName;
}

void ChatGenerateStreamHandler::runGenerateTask(
    AiProvider& aiProvider, std::shared_ptr<StreamResponder> responder,
    std::uint32_t streamId, std::uint64_t requestId,
    chat::GenerateRequest request) {
    if (!responder) {
        return;
    }

    auto onChunk = [responder, streamId,
                    requestId](const chat::GenerateChunk& chunk)
        -> AiProvider::Status {
        return responder->sendData(streamId, requestId, chunk);
    };

    auto shouldStop = [responder, streamId]() -> AiProvider::Status {
        return responder->shouldStop(streamId);
    };

    const AiProvider::Status status =
        aiProvider.generateStreaming(request, std::move(onChunk),
                                     std::move(shouldStop));

    if (status.ok()) {
        (void)responder->sendEnd(streamId, requestId, meta::RPC_OK, "");
        return;
    }

    if (status.code == AiProvider::StatusCode::kCancelled ||
        status.code == AiProvider::StatusCode::kConsumerStopped) {
        LOG_INFO << "[ChatGenerateStreamHandler] generate task stopped, streamId="
                 << streamId << ", reason=" << status.errorText;
        return;
    }

    const meta::RpcErrorCode code = aiStatusToErrorCode(status.code);
    const std::string text =
        defaultErrorText(status.errorText, "AI generate task failed");

    LOG_WARN << "[ChatGenerateStreamHandler] generate task failed, streamId="
             << streamId << ", error=" << text;

    (void)responder->sendEnd(streamId, requestId, code, text);
}

meta::RpcErrorCode ChatGenerateStreamHandler::aiStatusToErrorCode(
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

meta::RpcErrorCode ChatGenerateStreamHandler::executorSubmitResultToErrorCode(
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

std::string ChatGenerateStreamHandler::defaultErrorText(
    const std::string& text, const char* fallback) {
    if (!text.empty()) {
        return text;
    }

    return fallback == nullptr ? "unknown error" : fallback;
}

}  // namespace novanet::rpc
