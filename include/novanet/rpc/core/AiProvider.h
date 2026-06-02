#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "chat.pb.h"

namespace novanet::rpc {

/*
 * AiProvider 是 NovaNet Phase 4 中 AI streaming 生成能力的抽象接口。
 *
 * 设计目标：
 * - 面向生产级 / 企业级 streaming 语义；
 * - 不返回 vector<GenerateChunk>；
 * - 每生成一个 chunk，就通过 ChunkSink 推给上层；
 * - 支持 cancel / timeout / backpressure / consumer stop；
 * - RpcDispatcher 只依赖 AiProvider&，不依赖 FakeAiProvider；
 * - AiProvider 不认识 RpcMessage / FrameType / TcpConnection / Buffer /
 * Socket。
 *
 * 分层边界：
 *
 * AiProvider 只负责：
 *   GenerateRequest -> GenerateChunk*
 *
 * AiProvider 不负责：
 *   GenerateChunk -> STREAM_DATA
 *   STREAM_END
 *   STREAM_CANCEL
 *   RpcMessage
 *   TcpConnection::send
 *   EventLoop::queueInLoop
 *
 * 上述协议封装和发送由 StreamResponder / RpcServer 完成。
 */
class AiProvider {
public:
    enum class StatusCode {
        kOk = 0,

        // 请求本身非法，例如没有 user message。
        kInvalidRequest,

        // 上层取消了 stream。
        kCancelled,

        // 生成过程超时或 stream idle timeout。
        kTimeout,

        // 下游发送侧发生背压，不能继续生产。
        kBackpressure,

        // ChunkSink 返回失败，表示消费者要求停止。
        kConsumerStopped,

        // Provider 内部错误。
        kProviderError,
    };

    struct Status {
        StatusCode code{StatusCode::kOk};
        std::string errorText;

        [[nodiscard]] bool ok() const noexcept {
            return code == StatusCode::kOk;
        }

        [[nodiscard]] static Status success() {
            return Status{};
        }

        [[nodiscard]] static Status invalidRequest(std::string error) {
            Status status;
            status.code = StatusCode::kInvalidRequest;
            status.errorText = std::move(error);
            return status;
        }

        [[nodiscard]] static Status cancelled(
            std::string error = "stream cancelled") {
            Status status;
            status.code = StatusCode::kCancelled;
            status.errorText = std::move(error);
            return status;
        }

        [[nodiscard]] static Status timeout(
            std::string error = "stream timeout") {
            Status status;
            status.code = StatusCode::kTimeout;
            status.errorText = std::move(error);
            return status;
        }

        [[nodiscard]] static Status backpressure(
            std::string error = "stream backpressure") {
            Status status;
            status.code = StatusCode::kBackpressure;
            status.errorText = std::move(error);
            return status;
        }

        [[nodiscard]] static Status consumerStopped(std::string error) {
            Status status;
            status.code = StatusCode::kConsumerStopped;
            status.errorText = std::move(error);
            return status;
        }

        [[nodiscard]] static Status providerError(std::string error) {
            Status status;
            status.code = StatusCode::kProviderError;
            status.errorText = std::move(error);
            return status;
        }
    };

    /*
     * ChunkSink 是 chunk 消费回调。
     *
     * Provider 每生成一个 GenerateChunk，就调用一次 ChunkSink。
     *
     * 返回值语义：
     * - Status::success()：上层成功接收该 chunk，Provider 可以继续生成；
     * - 非 OK：上层要求停止生成，例如 backpressure / cancel / stream closed。
     *
     * 注意：
     * - ChunkSink 不应该长时间阻塞；
     * - ChunkSink 通常由 RpcDispatcher / StreamResponder 适配；
     * - ChunkSink 负责把 GenerateChunk 交给协议层，但 AiProvider
     * 不知道协议层细节。
     */
    using ChunkSink =
        std::function<Status(const novanet::ai::chat::GenerateChunk& chunk)>;

    /*
     * StopChecker 用于让 Provider 在生成过程中主动检查是否应该停止。
     *
     * 返回值语义：
     * - Status::success()：继续生成；
     * - 非 OK：停止生成，并把该 Status 作为 generateStreaming() 的返回值。
     *
     * 常见停止原因：
     * - stream cancelled
     * - stream timeout
     * - connection closed
     * - backpressure
     */
    using StopChecker = std::function<Status()>;

    virtual ~AiProvider() = default;

    AiProvider(const AiProvider&) = delete;
    AiProvider& operator=(const AiProvider&) = delete;

    AiProvider(AiProvider&&) = delete;
    AiProvider& operator=(AiProvider&&) = delete;

    /*
     * 真正的 streaming 生成接口。
     *
     * 语义：
     * - 不返回 vector；
     * - 不一次性生成完整结果；
     * - 每生成一个 chunk，就调用 onChunk(chunk)；
     * - onChunk 返回非 OK 时，Provider 必须尽快停止；
     * - shouldStop 返回非 OK 时，Provider 必须尽快停止；
     * - 函数返回值表示整个生成过程的最终状态。
     *
     * 线程模型：
     * - 该函数不应在 EventLoop 线程中直接执行慢生成任务；
     * - 生产级 Phase 4 中应由 AiExecutor worker 线程调用；
     * - Provider 实现不应直接跨线程操作 TcpConnection。
     *
     * 回调顺序：
     * - 默认要求同一次 generateStreaming() 调用内，onChunk 按生成顺序串行调用；
     * - 不允许 Provider 在没有明确文档说明的情况下并发调用同一个 onChunk。
     */
    [[nodiscard]] virtual Status generateStreaming(
        const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
        StopChecker shouldStop) = 0;

protected:
    AiProvider() = default;
};

[[nodiscard]] inline std::string_view toString(
    AiProvider::StatusCode code) noexcept {
    switch (code) {
        case AiProvider::StatusCode::kOk:
            return "kOk";

        case AiProvider::StatusCode::kInvalidRequest:
            return "kInvalidRequest";

        case AiProvider::StatusCode::kCancelled:
            return "kCancelled";

        case AiProvider::StatusCode::kTimeout:
            return "kTimeout";

        case AiProvider::StatusCode::kBackpressure:
            return "kBackpressure";

        case AiProvider::StatusCode::kConsumerStopped:
            return "kConsumerStopped";

        case AiProvider::StatusCode::kProviderError:
            return "kProviderError";

        default:
            return "UnknownAiProviderStatus";
    }
}

}  // namespace novanet::rpc