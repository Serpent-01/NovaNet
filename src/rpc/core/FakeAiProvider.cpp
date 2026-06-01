#include "novanet/rpc/core/FakeAiProvider.h"

#include <utility>

namespace novanet::rpc {

FakeAiProvider::Result FakeAiProvider::generate(
    const novanet::ai::chat::GenerateRequest& request) const {
    const std::string userText = extractLastUserMessage(request);

    if (userText.empty()) {
        return Result::failure("GenerateRequest has no user message");
    }

    /*
     * 这是 FakeAiProvider 的默认模拟行为。
     * 注意：这里生成 3 个 chunk 只是 provider 的默认输出，
     * 不是 NovaNet Streaming RPC 的协议能力上限。
     */
    std::vector<novanet::ai::chat::GenerateChunk> chunks;
    chunks.reserve(3);

    {
        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(0);
        chunk.set_delta("Echo: ");
        chunks.push_back(std::move(chunk));
    }

    {
        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(1);
        chunk.set_delta(userText);
        chunks.push_back(std::move(chunk));
    }

    {
        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(2);
        chunk.set_delta(" [fake-llm]");
        chunk.set_finish_reason("stop");
        chunks.push_back(std::move(chunk));
    }

    return Result::success(std::move(chunks));
}

std::string FakeAiProvider::extractLastUserMessage(
    const novanet::ai::chat::GenerateRequest& request) {
    for (int i = request.messages_size() - 1; i >= 0; --i) {
        const auto& message = request.messages(i);

        if (message.role() == "user" && !message.content().empty()) {
            return message.content();
        }
    }

    if (request.messages_size() > 0) {
        return request.messages(request.messages_size() - 1).content();
    }

    return {};
}

} // namespace novanet::rpc