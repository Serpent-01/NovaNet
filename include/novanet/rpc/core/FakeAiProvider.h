#pragma once

#include "chat.pb.h"

#include <string>
#include <vector>

namespace novanet::rpc {

/*
 * FakeAiProvider 是 Phase 4 的本地模拟 AI 生成器。
 *
 * 职责：
 * - 根据 chat::GenerateRequest 生成若干 GenerateChunk；
 * - 不访问网络；
 * - 不调用真实 LLM；
 * - 不依赖 TcpConnection / Buffer / RpcCodec；
 * - 后续真实 AI 接入时，可以替换为 AiProvider / Gateway 层。
 */
class FakeAiProvider final {
public:
    struct Result {
        bool ok{false};
        std::string errorText;
        std::vector<novanet::ai::chat::GenerateChunk> chunks;

        [[nodiscard]] static Result
        success(std::vector<novanet::ai::chat::GenerateChunk> outputChunks) {
            Result result;
            result.ok = true;
            result.chunks = std::move(outputChunks);
            return result;
        }

        [[nodiscard]] static Result failure(std::string error) {
            Result result;
            result.ok = false;
            result.errorText = std::move(error);
            return result;
        }
    };

    FakeAiProvider() = default;
    ~FakeAiProvider() = default;

    FakeAiProvider(const FakeAiProvider&) = delete;
    FakeAiProvider& operator=(const FakeAiProvider&) = delete;

    [[nodiscard]] Result
    generate(const novanet::ai::chat::GenerateRequest& request) const;

private:
    [[nodiscard]] static std::string
    extractLastUserMessage(const novanet::ai::chat::GenerateRequest& request);
};

} // namespace novanet::rpc