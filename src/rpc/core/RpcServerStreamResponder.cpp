#include "novanet/rpc/core/RpcServerStreamResponder.h"

#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"
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
    if (!connection) {
        LOG_ERROR << "[RpcServerStreamResponder] create failed: connection is null";
        return nullptr;
    }

    if (!streamManager) {
        LOG_ERROR << "[RpcServerStreamResponder] create failed: streamManager is null";
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

    AiProvider::Status reserveStatus = tryReservePendingDataMessage(streamId);
    if (!reserveStatus.ok()) {
        return reserveStatus;
    }

    const std::uint64_t sequence = nextSequence(streamId);

    auto message = buildStreamDataMessage(streamId, requestId, sequence, chunk);
    if (!message.has_value()) {
        releasePendingDataMessage();
        LOG_ERROR << "[RpcServerStreamResponder] failed to build STREAM_DATA, streamId="
                  << streamId;
        return AiProvider::Status::consumerStopped("failed to build STREAM_DATA");
    }

    AiProvider::Status enqueueStatus = enqueueMessage(std::move(*message), true);
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

    const std::uint64_t totalSequences = currentSequenceCount(streamId);

    auto message = buildStreamEndMessage(streamId, requestId, errorCode,
                                         std::move(errorText), totalSequences);
    if (!message.has_value()) {
        LOG_ERROR << "[RpcServerStreamResponder] failed to build STREAM_END, streamId="
                  << streamId;
        return AiProvider::Status::consumerStopped("failed to build STREAM_END");
    }

    return enqueueMessage(std::move(*message), false);
}

AiProvider::Status RpcServerStreamResponder::sendError(std::uint32_t streamId,
                                                       std::uint64_t requestId,
                                                       meta::RpcErrorCode errorCode,
                                                       std::string errorText) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    auto message =
        buildErrorFrameMessage(streamId, requestId, errorCode, std::move(errorText));
    if (!message.has_value()) {
        LOG_ERROR << "[RpcServerStreamResponder] failed to build ERROR_FRAME, streamId="
                  << streamId;
        return AiProvider::Status::consumerStopped("failed to build ERROR_FRAME");
    }

    return enqueueMessage(std::move(*message), false);
}

AiProvider::Status RpcServerStreamResponder::shouldStop(std::uint32_t streamId) const {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return AiProvider::Status::cancelled(closeReason_);
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

    if (session->cancelled()) {
        return AiProvider::Status::cancelled(session->cancelReason());
    }

    if (session->closed()) {
        return AiProvider::Status::consumerStopped("stream closed");
    }

    if (!session->canSendData()) {
        return AiProvider::Status::consumerStopped("stream cannot send data");
    }

    return AiProvider::Status::success();
}

void RpcServerStreamResponder::markConnectionClosed(std::string reason) {
    connectionClosed_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        closeReason_ = std::move(reason);
        nextSequenceByStream_.clear();
        backpressuredStreams_.clear();
    }

    pendingDataMessages_.store(0, std::memory_order_release);

    if (streamManager_) {
        static_cast<void>(streamManager_->cancelAll(closeReason_));
    }

    LOG_INFO << "[RpcServerStreamResponder] connection closed, reason=" << closeReason_;
}

AiProvider::Status RpcServerStreamResponder::tryReservePendingDataMessage(
    std::uint32_t streamId) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    if (options_.maxPendingDataMessages == 0) {
        pendingDataMessages_.fetch_add(1, std::memory_order_acq_rel);
        return AiProvider::Status::success();
    }

    std::size_t current = pendingDataMessages_.load(std::memory_order_acquire);

    while (true) {
        if (current >= options_.maxPendingDataMessages) {
            markStreamBackpressured(streamId);
            LOG_WARN << "[RpcServerStreamResponder] pending DATA queue full, streamId="
                     << streamId << ", pending=" << current;
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

AiProvider::Status RpcServerStreamResponder::enqueueMessage(RpcMessage message,
                                                            bool isDataMessage) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return AiProvider::Status::cancelled("connection closed");
    }

    auto connection = connection_.lock();
    if (!connection) {
        markConnectionClosed("connection expired");
        return AiProvider::Status::cancelled("connection expired");
    }

    auto* loop = connection->getLoop();
    if (loop == nullptr) {
        markConnectionClosed("connection loop is null");
        return AiProvider::Status::cancelled("connection loop is null");
    }

    auto self = shared_from_this();

    loop->queueInLoop([self, message = std::move(message), isDataMessage]() mutable {
        self->sendMessageInLoop(std::move(message), isDataMessage);
    });

    return AiProvider::Status::success();
}

void RpcServerStreamResponder::sendMessageInLoop(RpcMessage message, bool isDataMessage) {
    if (isDataMessage) {
        releasePendingDataMessage();
    }

    if (connectionClosed_.load(std::memory_order_acquire)) {
        return;
    }

    auto connection = connection_.lock();
    if (!connection || !connection->connected()) {
        markConnectionClosed("connection closed before send");
        return;
    }

    if (message.frameType() == FrameType::STREAM_DATA) {
        AiProvider::Status status = shouldStop(message.streamId());
        if (!status.ok()) {
            LOG_WARN << "[RpcServerStreamResponder] drop late STREAM_DATA, streamId="
                     << message.streamId() << ", reason=" << status.errorText;
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
            (void)streamManager_->markLocalEnd(message.streamId());
        }

        eraseStreamLocalState(message.streamId());

        LOG_INFO << "[RpcServerStreamResponder] STREAM_END sent, streamId="
                 << message.streamId();
    }
}

void RpcServerStreamResponder::sendRawMessageInLoop(const RpcMessage& message) {
    auto connection = connection_.lock();
    if (!connection || !connection->connected()) {
        markConnectionClosed("connection closed during raw send");
        return;
    }

    novanet::net::Buffer out;
    if (!codec_.encode(message, out)) {
        LOG_ERROR << "[RpcServerStreamResponder] encode failed, frameType="
                  << static_cast<int>(message.type())
                  << ", streamId=" << message.streamId();
        markConnectionClosed("encode failed");
        return;
    }

    connection->send(out.retrieveAllAsString());
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
                                            reason, totalSequences);
    if (endMessage.has_value()) {
        sendRawMessageInLoop(*endMessage);
    }

    eraseStreamLocalState(streamId);

    LOG_WARN << "[RpcServerStreamResponder] stream backpressure, streamId=" << streamId
             << ", reason=" << reason;
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
