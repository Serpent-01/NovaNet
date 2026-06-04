#pragma once

#include <string>

#include "chat.pb.h"
#include "novanet/rpc/core/AiExecutor.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/core/StreamMethodHandler.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * ChatGenerateStreamHandler 适配 Phase 4 内置 chat streaming 示例：
 *
 *   novanet.ai.chat.ChatService.Generate
 *
 * 它只负责把 GenerateRequest 解析为 AiProvider streaming 任务，并提交到
 * AiExecutor；不会读写 socket，也不会直接操作 TcpConnection。
 */
class ChatGenerateStreamHandler final : public StreamMethodHandler {
public:
    ChatGenerateStreamHandler(AiProvider& aiProvider, AiExecutor& aiExecutor);

    ChatGenerateStreamHandler(const ChatGenerateStreamHandler&) = delete;
    ChatGenerateStreamHandler& operator=(const ChatGenerateStreamHandler&) = delete;

    [[nodiscard]] std::string serviceName() const override;
    [[nodiscard]] std::string methodName() const override;

    [[nodiscard]] RpcStatus start(StartContext context) override;

    [[nodiscard]] static const char* serviceFullName() noexcept;
    [[nodiscard]] static const char* generateMethodName() noexcept;

private:
    static void runGenerateTask(AiProvider& aiProvider,
                                std::shared_ptr<StreamResponder> responder,
                                std::uint32_t streamId,
                                std::uint64_t requestId,
                                novanet::ai::chat::GenerateRequest request);

    [[nodiscard]] static meta::RpcErrorCode aiStatusToErrorCode(
        AiProvider::StatusCode code) noexcept;

    [[nodiscard]] static meta::RpcErrorCode executorSubmitResultToErrorCode(
        AiExecutor::SubmitResult result) noexcept;

    [[nodiscard]] static std::string defaultErrorText(const std::string& text,
                                                      const char* fallback);

private:
    AiProvider& aiProvider_;
    AiExecutor& aiExecutor_;
};

}  // namespace novanet::rpc
