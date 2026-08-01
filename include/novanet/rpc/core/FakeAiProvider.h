#pragma once

#include <string>
#include <vector>

#include "novanet/rpc/core/AiProvider.h"

namespace novanet::rpc {

/*
 * FakeAiProvider 是 AiProvider 的本地模拟实现。
 *
 * 设计目标：
 * - 用于 Phase 4 chat streaming example / tests；
 * - 不访问网络；
 * - 不调用真实 LLM；
 * - 不使用 curl / fork / HTTP SSE；
 * - 不依赖 RpcMessage / TcpConnection / Buffer；
 * - 通过 AiProvider::ChunkSink 逐 chunk 输出；
 * - 支持 shouldStop，用于 cancel / timeout / backpressure / connection closed。
 * - 使用确定性的本地回答模板，便于重复执行稳定性测试；
 * - 按自然语义拆分多个 chunk，模拟真实 AI 的流式响应。
 */
class FakeAiProvider final : public AiProvider {
public:
    FakeAiProvider() = default;
    ~FakeAiProvider() override = default;

    FakeAiProvider(const FakeAiProvider&) = delete;
    FakeAiProvider& operator=(const FakeAiProvider&) = delete;

    FakeAiProvider(FakeAiProvider&&) = delete;
    FakeAiProvider& operator=(FakeAiProvider&&) = delete;

    /*
     * 根据 GenerateRequest 逐 chunk 输出 GenerateChunk。
     *
     * 每生成一个 chunk，就调用一次 onChunk。
     */
    [[nodiscard]] Status generateStreaming(
        const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
        StopChecker shouldStop) override;

private:
    [[nodiscard]] static std::string extractLastUserMessage(
        const novanet::ai::chat::GenerateRequest& request);

    [[nodiscard]] static std::vector<std::string> buildResponseChunks(
        const std::string& userText);

    [[nodiscard]] static std::string summarizeQuestion(
        const std::string& userText);

    [[nodiscard]] static bool isValidRole(const std::string& role) noexcept;
};

}  // namespace novanet::rpc
