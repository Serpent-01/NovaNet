#include "novanet/rpc/core/RpcServerStreamResponder.h"

#include <atomic>
#include <memory>
#include <utility>

#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/stream/StreamFrame.h"

namespace novanet::rpc {

namespace chat = ::novanet::ai::chat;

std::shared_ptr<RpcServerStreamResponder> RpcServerStreamResponder::create(
    TcpConnectionPtr connection, std::shared_ptr<StreamManager> streamManager) {
    return create(std::move(connection), std::move(streamManager), Options{});
}

std::shared_ptr<RpcServerStreamResponder> RpcServerStreamResponder::create(
    TcpConnectionPtr connection, std::shared_ptr<StreamManager> streamManager,
    Options options) {
    if (!connection || !streamManager) {
        return nullptr;
    }
    return std::shared_ptr<RpcServerStreamResponder>(new RpcServerStreamResponder(
        std::move(connection), std::move(streamManager), options));
}

RpcServerStreamResponder::RpcServerStreamResponder(
    TcpConnectionPtr connection, std::shared_ptr<StreamManager> streamManager,
    Options options)
    : connection_(connection),
      streamManager_(std::move(streamManager)),
      options_(options) {
}

AiProvider::Status RpcServerStreamResponder::sendData(std::uint32_t streamId,
                                                      std::uint64_t requestId,
                                                      const chat::GenerateChunk& chunk) {
    AiProvider::Status stopStatus = shouldStop(streamId);
    if (!stopStatus.ok()) {
        return stopStatus;
    }

    AiProvider::Status reserveStatus = tryReservePendingDataMessage();

    if (!reserveStatus.ok()) {
        markStreamBackpressured(streamId);
        return reserveStatus;
    }

    const std::uint64_t sequence = nextSequence(streamId);

    auto message = buildStreamDataMessage(streamId, requestId, sequence, chunk);

    if (!message.has_value()) {
        releasePendingDataMessage();
        return AiProvider::Status::consumerStopped("failed to build STREAM_DATA message");
    }
    //投递到 EventLoop线程
    AiProvider::Status enqueueStatus = enqueueDataMessage(std::move(*message));
    if (!enqueueStatus.ok()) {
        releasePendingDataMessage();
        return enqueueStatus;
    }

    return AiProvider::Status::success();
}
AiProvider::Status RpcServerStreamResponder::sendEnd(std::uint32_t streamId,
                                                     std::uint64_t requestId,
                                                     meta::RpcErrorCode errorCode,
                                                     std::string errorText) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    /*
     * sendEnd 不提前 erase sequence。
     * totalSequences 表示目前已接受/编号的 DATA 数。
     * 真正清理放在 STREAM_END 进入 EventLoop 后执行。
     */
    const std::uint64_t totalSequences = currentSequenceCount(streamId);

    auto message = buildStreamEndMessage(streamId, requestId, errorCode,
                                         std::move(errorText), totalSequences);
    if (!message.has_value()) {
        return AiProvider::Status::consumerStopped("failed to build STREAM_END message");
    }

    return enqueueControlMessage(std::move(*message));
}

AiProvider::Status RpcServerStreamResponder::sendError(std::uint32_t streamId,
                                                       std::uint64_t requestId,
                                                       meta::RpcErrorCode errorCode,
                                                       std::string errorText) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }
    /*
     * ERROR_FRAME 是协议级错误通知，不默认终止 stream。
     * stream 级终止错误应使用 sendEnd(errorCode, errorText)。
     */
    auto message =
        buildErrorFrameMessage(streamId, requestId, errorCode, std::move(errorText));

    if (!message.has_value()) {
        return AiProvider::Status::consumerStopped("failed to build ERROR_FRAME message");
    }
    return enqueueControlMessage(std::move(*message));
}

AiProvider::Status RpcServerStreamResponder::shouldStop(std::uint32_t streamId) const {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }
    if (isStreamBackpressured(streamId)) {
        return AiProvider::Status::backpressure("stream backpressured");
    }

    if (!streamManager_) {
        return AiProvider::Status::cancelled("stream manager unavailable");
    }
    auto session = streamManager_->findStream(streamId);

    if (!session) {
        return AiProvider::Status::cancelled("stream not found");
    }
    // stream 是否 cancelled
    if (session->cancelled()) {
        return AiProvider::Status::cancelled(session->cancelReason());
    }
    // stream 是否 closed
    if (session->closed()) {
        return AiProvider::Status::consumerStopped("stream closed");
    }

    // stream 是否 canSendData
    if (!session->canSendData()) {
        return AiProvider::Status::consumerStopped("stream cannot send data");
    }

    return AiProvider::Status::success();
}

void RpcServerStreamResponder::markConnectionClosed() {
    connectionClosed_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        nextSequenceByStream_.clear();
        backpressuredStreams_.clear();
    }
    pendingDataMessages_.store(0, std::memory_order_release);

    if (streamManager_) {
        /*
         * 要求 StreamManager::cancelAll 内部不能持锁执行 callback。
         */
        static_cast<void>(streamManager_->cancelAll("connection closed"));
    }
}

AiProvider::Status RpcServerStreamResponder::tryReservePendingDataMessage() {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    if (options_.maxPendingDataMessages == 0) {
        pendingDataMessages_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::size_t current = pendingDataMessages_.load(std::memory_order_acquire);

    while (true) {
        if (current >= options_.maxPendingDataMessages) {
            return AiProvider::Status::backpressure(
                "stream responder pending DATA queue full");
        }
        if (pendingDataMessages_.compare_exchange_weak(current, current + 1,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            return AiProvider::Status::success();
        }
    }
}

void RpcServerStreamResponder::releasePendingDataMessage() noexcept {
    std::size_t current = pendingDataMessages_.load(std::memory_order_acquire);

    while (current > 0) {
        if (pendingDataMessages_.compare_exchange_weak(current, current - 1,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            return;
        }
    }
}

std::uint64_t RpcServerStreamResponder::nextSequence(std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(stateMutex_);

    auto& next = nextSequenceByStream_[streamId];
    const std::uint64_t current = next;
    ++next;

    return current;
}

std::uint64_t RpcServerStreamResponder::currentSequenceCount(
    std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);

    auto it = nextSequenceByStream_.find(streamId);
    if (it == nextSequenceByStream_.end()) {
        return 0;
    }

    return it->second;
}

void RpcServerStreamResponder::eraseStreamLocalState(std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(stateMutex_);

    nextSequenceByStream_.erase(streamId);
    backpressuredStreams_.erase(streamId);
}

void RpcServerStreamResponder::markStreamBackpressured(std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    backpressuredStreams_.insert(streamId);
}

bool RpcServerStreamResponder::isStreamBackpressured(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return backpressuredStreams_.find(streamId) != backpressuredStreams_.end();
}

AiProvider::Status RpcServerStreamResponder::enqueueDataMessage(RpcMessage message) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    auto connection = connection_.lock();
    if (!connection) {
        connectionClosed_.store(true, std::memory_order_release);
        return AiProvider::Status::cancelled("connection closed");
    }

    auto* loop = connection->getLoop();
    if (loop == nullptr) {
        connectionClosed_.store(true, std::memory_order_release);
        return AiProvider::Status::cancelled("connection loop is null");
    }

    auto self = shared_from_this();

    loop->queueInLoop([self, message = std::move(message)]() mutable {
        self->sendMessageInLoop(std::move(message));
    });

    return AiProvider::Status::success();
}

AiProvider::Status RpcServerStreamResponder::enqueueControlMessage(RpcMessage message) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    auto connection = connection_.lock();
    if (!connection) {
        connectionClosed_.store(true, std::memory_order_release);
        return AiProvider::Status::cancelled("connection closed");
    }

    auto* loop = connection->getLoop();
    if (loop == nullptr) {
        connectionClosed_.store(true, std::memory_order_release);
        return AiProvider::Status::cancelled("connection loop is null");
    }

    auto self = shared_from_this();

    loop->queueInLoop([self, message = std::move(message)]() mutable {
        self->sendMessageInLoop(std::move(message));
    });

    return AiProvider::Status::success();
}

void RpcServerStreamResponder::sendMessageInLoop(RpcMessage message) {
    if (message.frameType() == FrameType::STREAM_DATA) {
        releasePendingDataMessage();
    }

    if (connectionClosed_.load(std::memory_order_acquire)) {
        return;
    }

    auto connection = connection_.lock();
    if (!connection || !connection->connected()) {
        connectionClosed_.store(true, std::memory_order_release);
        return;
    }

    /*
     * 已经排队的 STREAM_DATA 在真正发送前必须重新检查 stream 状态。
     * 这样可以避免 cancel/backpressure/timeout 后，旧 DATA 继续发送。
     */
    if (message.frameType() == FrameType::STREAM_DATA) {
        AiProvider::Status status = shouldStop(message.streamId());
        if (!status.ok()) {
            return;
        }

        if (options_.highWaterMarkBytes != 0 &&
            connection->outputBufferSize() >= options_.highWaterMarkBytes) {
            handleBackpressureInLoop(message.streamId(), message.requestId(),
                                     "stream backpressure");
            return;
        }
    }

    sendRawMessageInLoop(message);

    if (message.frameType() == FrameType::STREAM_END) {
        if (streamManager_) {
            /*
             * 不无条件 removeStream。
             * markLocalEnd 负责状态转换。
             * 如果 StreamManager::markLocalEnd 内部在 terminal 时清理，
             * 这里不再额外 remove，避免破坏半关闭语义。
             */
            (void)streamManager_->markLocalEnd(message.streamId());
        }

        eraseStreamLocalState(message.streamId());
    }
}

void RpcServerStreamResponder::sendRawMessageInLoop(const RpcMessage& message) {
    auto connection = connection_.lock();
    if (!connection || !connection->connected()) {
        connectionClosed_.store(true, std::memory_order_release);
        return;
    }

    novanet::net::Buffer buffer;
    if (!codec_.encode(message, buffer)) {
        /*
         * encode 失败通常说明内部构造了非法 RpcMessage。
         * 这里不能抛异常，也不能递归发送 ERROR_FRAME。
         * 标记连接关闭倾向，让 worker 尽快停止。
         */
        connectionClosed_.store(true, std::memory_order_release);
        return;
    }

    /*
     * 如果你的 TcpConnection 支持 send(Buffer*)，可以改成更高效版本。
     * 当前使用 string 版本，兼容已有 conn->send(std::string)。
     */
    connection->send(buffer.retrieveAllAsString());
}

void RpcServerStreamResponder::handleBackpressureInLoop(std::uint32_t streamId,
                                                        std::uint64_t requestId,
                                                        std::string reason) {
    markStreamBackpressured(streamId);

    const std::uint64_t totalSequences = currentSequenceCount(streamId);

    if (streamManager_) {
        auto session = streamManager_->removeStream(streamId);
        if (session) {
            (void)session->markCancelled(reason);
        }
    }

    auto endMessage = buildStreamEndMessage(streamId, requestId, meta::RPC_BACKPRESSURE,
                                            std::move(reason), totalSequences);
    if (endMessage.has_value()) {
        sendRawMessageInLoop(*endMessage);
    }

    eraseStreamLocalState(streamId);
}

std::optional<RpcMessage> RpcServerStreamResponder::buildStreamDataMessage(
    std::uint32_t streamId, std::uint64_t requestId, std::uint64_t sequence,
    const chat::GenerateChunk& chunk) const {
    std::string chunkBytes;
    if (!chunk.SerializeToString(&chunkBytes)) {
        return std::nullopt;
    }

    meta::StreamDataMeta dataMeta;
    dataMeta.set_sequence(sequence);
    dataMeta.set_data(std::move(chunkBytes));

    std::string payload;
    if (!dataMeta.SerializeToString(&payload)) {
        return std::nullopt;
    }

    auto frame = StreamFrame::makeData(streamId, requestId, std::move(payload));
    if (!frame.valid()) {
        return std::nullopt;
    }

    return frame.releaseMessage();
}

std::optional<RpcMessage> RpcServerStreamResponder::buildStreamEndMessage(
    std::uint32_t streamId, std::uint64_t requestId, meta::RpcErrorCode errorCode,
    std::string errorText, std::uint64_t totalSequences) const {
    meta::StreamEndMeta endMeta;
    endMeta.set_error_code(errorCode);
    endMeta.set_error_text(std::move(errorText));
    endMeta.set_total_sequences(totalSequences);

    std::string payload;
    if (!endMeta.SerializeToString(&payload)) {
        return std::nullopt;
    }

    auto frame = StreamFrame::makeEnd(streamId, requestId, std::move(payload));
    if (!frame.valid()) {
        return std::nullopt;
    }

    return frame.releaseMessage();
}

std::optional<RpcMessage> RpcServerStreamResponder::buildErrorFrameMessage(
    std::uint32_t streamId, std::uint64_t requestId, meta::RpcErrorCode errorCode,
    std::string errorText) const {
    meta::ErrorFrameMeta errorMeta;
    errorMeta.set_error_code(errorCode);
    errorMeta.set_error_text(std::move(errorText));

    std::string payload;
    if (!errorMeta.SerializeToString(&payload)) {
        return std::nullopt;
    }

    RpcMessage message(FrameType::ERROR_FRAME, streamId, requestId, std::move(payload));

    if (!message.valid()) {
        return std::nullopt;
    }

    return message;
}

}  // namespace novanet::rpc