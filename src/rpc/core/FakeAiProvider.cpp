#include "novanet/rpc/core/FakeAiProvider.h"

#include <cstdint>
#include <string>
#include <utility>

namespace novanet::rpc {

AiProvider::Status FakeAiProvider::generateStreaming(
    const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
    StopChecker shouldStop) {
    if (!onChunk) {
        return Status::invalidRequest("ChunkSink is empty");
    }

    /*
     * Phase 4 不接真实模型，但请求仍然要做基本校验。
     * FakeAiProvider 只模拟业务生成，不负责 RPC 协议校验。
     */
    const std::string userText = extractLastUserMessage(request);
    if (userText.empty()) {
        return Status::invalidRequest("GenerateRequest has no user message");
    }

    /*
     * checkStop 用于统一处理 cancel / timeout / backpressure /
     * connection closed 等上层停止信号。
     *
     * shouldStop 返回非 OK，Provider 必须尽快停止生成。
     */
    auto checkStop = [&shouldStop]() -> Status {
        if (!shouldStop) {
            return Status::success();
        }

        Status status = shouldStop();
        if (!status.ok()) {
            return status;
        }

        return Status::success();
    };

    /*
     * emit 用于生成一个 GenerateChunk 并交给上层 ChunkSink。
     *
     * 注意：
     * - emit 前检查 shouldStop；
     * - onChunk 后再次由调用方继续下一轮；
     * - onChunk 返回非 OK，说明上层不希望继续生成。
     */
    auto emit = [&](std::uint32_t index, std::string delta,
                    std::string finishReason = "") -> Status {
        Status stopStatus = checkStop();
        if (!stopStatus.ok()) {
            return stopStatus;
        }

        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(index);
        chunk.set_delta(std::move(delta));
        chunk.set_finish_reason(std::move(finishReason));

        Status sinkStatus = onChunk(chunk);
        if (!sinkStatus.ok()) {
            return sinkStatus;
        }

        return Status::success();
    };

    /*
     * 默认 fake 输出：
     *
     *   chunk0: "Echo: "
     *   chunk1: 用户最后一条 user message
     *   chunk2: " [fake-llm]" + finish_reason="stop"
     *
     * 这只是 FakeAiProvider 的行为，不是协议层固定 DATA x3。
     */
    {
        Status status = emit(0, "Echo: ");
        if (!status.ok()) {
            return status;
        }
    }

    {
        Status status = emit(1, userText);
        if (!status.ok()) {
            return status;
        }
    }

    {
        Status status = emit(2, " [fake-llm]", "stop");
        if (!status.ok()) {
            return status;
        }
    }

    return Status::success();
}

std::string FakeAiProvider::extractLastUserMessage(
    const novanet::ai::chat::GenerateRequest& request) {
    /*
     * 优先查找最后一条 role == "user" 的消息。
     * 这是 Chat API 风格的基本语义。
     */
    for (int i = request.messages_size() - 1; i >= 0; --i) {
        const auto& message = request.messages(i);

        if (message.role() == "user" && !message.content().empty()) {
            return message.content();
        }
    }

    /*
     * 兜底：如果没有 role=user，但最后一条消息有内容，
     * 仍然允许 fake provider 回显。
     *
     * 这样方便测试，但真实 Provider 可以更严格。
     */
    if (request.messages_size() > 0) {
        const auto& last = request.messages(request.messages_size() - 1);
        if (!last.content().empty()) {
            return last.content();
        }
    }

    return {};
}

bool FakeAiProvider::isValidRole(const std::string& role) noexcept {
    return role == "system" || role == "user" || role == "assistant" ||
           role == "tool";
}

}  // namespace novanet::rpc