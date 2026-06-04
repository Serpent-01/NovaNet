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
#include "novanet/rpc/core/StreamResponder.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * RpcServerStreamResponder 是服务端 StreamResponder 实现。
 *
 * 每个 TcpConnection 一个 responder。
 * 它负责：
 * - GenerateChunk -> StreamDataMeta -> RpcMessage(STREAM_DATA)
 * - StreamEndMeta -> RpcMessage(STREAM_END)
 * - ErrorFrameMeta -> RpcMessage(ERROR_FRAME)
 * - queueInLoop 回连接所属 EventLoop
 * - EventLoop 内 encode/send
 * - backpressure 检查
 * - connection closed 后阻止 late DATA
 */
class RpcServerStreamResponder final
    : public StreamResponder,
      public std::enable_shared_from_this<RpcServerStreamResponder> {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;

    struct Options {
        std::size_t highWaterMarkBytes{8 * 1024 * 1024};
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

    void markConnectionClosed(std::string reason) override;

private:
    RpcServerStreamResponder(TcpConnectionPtr connection,
                             std::shared_ptr<StreamManager> streamManager,
                             Options options);

    [[nodiscard]] AiProvider::Status tryReservePendingDataMessage(std::uint32_t streamId);

    void releasePendingDataMessage() noexcept;

    [[nodiscard]] std::uint64_t nextSequence(std::uint32_t streamId);
    [[nodiscard]] std::uint64_t currentSequenceCount(std::uint32_t streamId) const;

    void eraseStreamLocalState(std::uint32_t streamId);
    void markStreamBackpressured(std::uint32_t streamId);
    [[nodiscard]] bool isStreamBackpressured(std::uint32_t streamId) const;

    [[nodiscard]] AiProvider::Status enqueueMessage(RpcMessage message,
                                                    bool isDataMessage);

    void sendMessageInLoop(RpcMessage message, bool isDataMessage);
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

    RpcCodec codec_;
    Options options_;

    std::atomic<bool> connectionClosed_{false};
    std::atomic<std::size_t> pendingDataMessages_{0};

    mutable std::mutex stateMutex_;
    std::string closeReason_{"connection closed"};
    std::unordered_map<std::uint32_t, std::uint64_t> nextSequenceByStream_;
    std::unordered_set<std::uint32_t> backpressuredStreams_;
};

}  // namespace novanet::rpc