#pragma once

#include <string>

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
 *
 * 注意：
 * 默认输出 3 个 chunk 只是 FakeAiProvider 的测试行为，
 * 不是 NovaNet Streaming RPC 的协议能力上限。
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

    [[nodiscard]] static bool isValidRole(const std::string& role) noexcept;
};

}  // namespace novanet::rpc