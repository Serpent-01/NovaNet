#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "chat.pb.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/core/StreamResponder.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * RpcServerStreamResponder 是服务端 StreamResponder 的默认实现。
 *
 * 生命周期要求：
 * - 必须使用 create() 创建，返回 shared_ptr；
 * - AiExecutor task 中应捕获 std::shared_ptr<StreamResponder>；
 * - TcpConnection 使用 weak_ptr 保存，连接关闭后不会悬空访问；
 * - StreamManager 建议每个连接一个，并用 shared_ptr 托管。
 *
 * 线程模型：
 * - sendData/sendEnd/sendError 可以在 AiExecutor worker 线程调用；
 * - 这些函数不会直接调用 TcpConnection::send；
 * - 它们只负责构造 RpcMessage，然后 queueInLoop 到连接所属 EventLoop；
 * - 真正 encode/send 在 EventLoop 线程执行。
 *
 * backpressure 策略：
 * - maxPendingDataMessages 限制 worker 投递到 EventLoop 但尚未执行的 DATA 数；
 * - highWaterMarkBytes 检查 TcpConnection outputBuffer；
 * - 触发 backpressure 后只终止当前 stream，不误伤同连接其他 stream。
 */
class RpcServerStreamResponder final
    : public StreamResponder,
      public std::enable_shared_from_this<RpcServerStreamResponder> {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;

    struct Options {
        /*
         * outputBuffer 超过该阈值时，认为当前 stream 触发 backpressure。
         *
         * 0 表示禁用 outputBuffer high-water 检查。
         *
         * 注意：
         *   TcpConnection 需要提供 outputBufferSize()。
         *   该检查必须在连接所属 EventLoop 线程中执行。
         */
        std::size_t highWaterMarkBytes{8 * 1024 * 1024};

        /*
         * 已投递到 EventLoop 但尚未执行的 STREAM_DATA 数量上限。
         *
         * 这是为了防止 AiExecutor worker 生成过快，把 queueInLoop
         * pending functors 堆爆。
         *
         * 0 表示不限制。
         */
        std::size_t maxPendingDataMessages{1024};
    };

    [[nodiscard]] static std::shared_ptr<RpcServerStreamResponder> create(
        TcpConnectionPtr connection, std::shared_ptr<StreamManager> streamManager);

    [[nodiscard]] static std::shared_ptr<RpcServerStreamResponder> create(
        TcpConnectionPtr connection, std::shared_ptr<StreamManager> streamManager,
        Options options);

    ~RpcServerStreamResponder() override = default;

    RpcServerStreamResponder(const RpcServerStreamResponder&) = delete;
    RpcServerStreamResponder& operator=(const RpcServerStreamResponder&) = delete;

    RpcServerStreamResponder(RpcServerStreamResponder&&) = delete;
    RpcServerStreamResponder& operator=(RpcServerStreamResponder&&) = delete;

    [[nodiscard]] AiProvider::Status sendData(
        std::uint32_t streamId, std::uint64_t requestId,
        const novanet::ai::chat::GenerateChunk& chunk) override;

    [[nodiscard]] AiProvider::Status sendEnd(std::uint32_t streamId,
                                             std::uint64_t requestId,
                                             novanet::rpc::meta::RpcErrorCode errorCode,
                                             std::string errorText) override;

    [[nodiscard]] AiProvider::Status sendError(std::uint32_t streamId,
                                               std::uint64_t requestId,
                                               novanet::rpc::meta::RpcErrorCode errorCode,
                                               std::string errorText) override;

    [[nodiscard]] AiProvider::Status shouldStop(std::uint32_t streamId) const override;

    void markConnectionClosed() override;

private:
    RpcServerStreamResponder(TcpConnectionPtr connection,
                             std::shared_ptr<StreamManager> streamManager,
                             Options options);

    [[nodiscard]] AiProvider::Status tryReservePendingDataMessage();
    void releasePendingDataMessage() noexcept;

    [[nodiscard]] std::uint64_t nextSequence(std::uint32_t streamId);
    [[nodiscard]] std::uint64_t currentSequenceCount(std::uint32_t streamId) const;
    void eraseStreamLocalState(std::uint32_t streamId);

    void markStreamBackpressured(std::uint32_t streamId);
    [[nodiscard]] bool isStreamBackpressured(std::uint32_t streamId) const;

    [[nodiscard]] AiProvider::Status enqueueDataMessage(RpcMessage message);
    [[nodiscard]] AiProvider::Status enqueueControlMessage(RpcMessage message);

    void sendMessageInLoop(RpcMessage message);
    void sendRawMessageInLoop(const RpcMessage& message);

    void handleBackpressureInLoop(std::uint32_t streamId, std::uint64_t requestId,
                                  std::string reason);

    [[nodiscard]] std::optional<RpcMessage> buildStreamDataMessage(
        std::uint32_t streamId, std::uint64_t requestId, std::uint64_t sequence,
        const novanet::ai::chat::GenerateChunk& chunk) const;

    [[nodiscard]] std::optional<RpcMessage> buildStreamEndMessage(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText,
        std::uint64_t totalSequences) const;

    [[nodiscard]] std::optional<RpcMessage> buildErrorFrameMessage(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText) const;

private:
    std::weak_ptr<novanet::net::TcpConnection> connection_;
    std::shared_ptr<StreamManager> streamManager_;

    /*
     * RpcCodec 当前应保持无状态。
     * responder 内部持有一个 codec，避免引用 RpcServer 成员导致异步生命周期问题。
     */
    RpcCodec codec_;

    Options options_;

    std::atomic<bool> connectionClosed_{false};

    /*
     * 限制 queueInLoop 中尚未执行的 STREAM_DATA 数量。
     */
    std::atomic<std::size_t> pendingDataMessages_{0};

    mutable std::mutex stateMutex_;

    std::unordered_map<std::uint32_t, std::uint64_t> nextSequenceByStream_;
    std::unordered_set<std::uint32_t> backpressuredStreams_;
};

}  // namespace novanet::rpc