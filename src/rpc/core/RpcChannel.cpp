#include "novanet/rpc/core/RpcChannel.h"

#include <utility>
#include <vector>

#include "novanet/base/Logger.h"
#include "novanet/base/Timestamp.h"
#include "novanet/net/EventLoop.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/stream/StreamFrame.h"

namespace novanet::rpc {

namespace chat = ::novanet::ai::chat;
using novanet::base::timeDifference;
using novanet::base::Timestamp;

RpcChannel::RpcChannel(TcpConnectionPtr connection)
    : RpcChannel(std::move(connection), Options{}) {
}

RpcChannel::RpcChannel(TcpConnectionPtr connection, Options options)
    : connection_(std::move(connection)), options_(std::move(options)) {
    const auto now = Timestamp::now().microSecondsSinceEpoch();
    lastPingMicros_.store(now, std::memory_order_release);
    lastPongMicros_.store(now, std::memory_order_release);
}

RpcChannel::~RpcChannel() {
    stopTimers();
    onConnectionClosed("RpcChannel destroyed");
}

RpcStatus RpcChannel::callUnary(const std::string& serviceName,
                                const std::string& methodName,
                                const google::protobuf::Message& request,
                                google::protobuf::Message* response,
                                std::chrono::milliseconds timeout) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return RpcStatus::failure(meta::RPC_CONNECTION_CLOSED,
                                  "connection already closed");
    }

    if (serviceName.empty() || methodName.empty()) {
        return RpcStatus::failure(meta::RPC_BAD_REQUEST,
                                  "service_name or method_name is empty");
    }

    if (response == nullptr) {
        return RpcStatus::failure(meta::RPC_BAD_REQUEST,
                                  "response message is null");
    }

    std::string requestPayload;
    if (!request.SerializeToString(&requestPayload)) {
        return RpcStatus::failure(meta::RPC_SERIALIZE_REQUEST_FAILED,
                                  "serialize unary request failed");
    }

    const std::uint64_t requestId = nextRequestId();
    auto pendingCall = pendingCalls_.create(requestId);
    if (!pendingCall) {
        return RpcStatus::failure(meta::RPC_INTERNAL_ERROR,
                                  "failed to create pending unary call");
    }

    meta::UnaryRequestMeta requestMeta;
    requestMeta.set_service_name(serviceName);
    requestMeta.set_method_name(methodName);
    requestMeta.set_request_payload(std::move(requestPayload));

    std::string rpcPayload;
    if (!requestMeta.SerializeToString(&rpcPayload)) {
        static_cast<void>(pendingCalls_.remove(requestId));
        return RpcStatus::failure(meta::RPC_SERIALIZE_REQUEST_FAILED,
                                  "serialize UnaryRequestMeta failed");
    }

    RpcMessage message(FrameType::UNARY_REQUEST, 0, requestId,
                       std::move(rpcPayload));
    if (!message.valid()) {
        static_cast<void>(pendingCalls_.remove(requestId));
        return RpcStatus::failure(meta::RPC_INVALID_FRAME,
                                  "invalid unary request frame");
    }

    if (!sendRpcMessage(std::move(message))) {
        static_cast<void>(pendingCalls_.remove(requestId));
        return RpcStatus::failure(meta::RPC_CONNECTION_CLOSED,
                                  "send unary request failed");
    }

    const PendingCall::State state = pendingCall->waitFor(timeout);
    if (state == PendingCall::State::kTimeout) {
        static_cast<void>(pendingCalls_.remove(requestId));
        return RpcStatus::failure(meta::RPC_TIMEOUT, pendingCall->errorText());
    }

    if (state == PendingCall::State::kFailed) {
        return RpcStatus::failure(meta::RPC_UNKNOWN_ERROR,
                                  pendingCall->errorText());
    }

    if (state != PendingCall::State::kDone) {
        return RpcStatus::failure(meta::RPC_UNKNOWN_ERROR,
                                  "unary call finished with unexpected state");
    }

    if (!response->ParseFromString(pendingCall->responseBytes())) {
        return RpcStatus::failure(meta::RPC_PARSE_RESPONSE_FAILED,
                                  "parse unary response payload failed");
    }

    return RpcStatus::success();
}

RpcChannel::StreamHandle RpcChannel::openStream(const std::string& serviceName,
                                                const std::string& methodName,
                                                const google::protobuf::Message& request,
                                                StreamCallbacks callbacks) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return makeStreamError("connection already closed");
    }

    std::string requestPayload;
    if (!request.SerializeToString(&requestPayload)) {
        return makeStreamError("serialize stream request failed");
    }

    const std::uint64_t requestId = nextRequestId();
    const std::uint32_t streamId = nextStreamId();

    auto session =
        streamManager_.createStream(streamId, requestId, serviceName, methodName);
    if (!session) {
        return makeStreamError("create local stream failed");
    }

    if (!session->markLocalEnd()) {
        (void)streamManager_.removeStream(streamId);
        return makeStreamError("mark local end failed");
    }

    saveCallbacks(streamId, std::move(callbacks));

    if (!sendStreamOpenMessage(streamId, requestId, serviceName, methodName,
                               requestPayload)) {
        eraseCallbacks(streamId);
        (void)streamManager_.removeStream(streamId);
        return makeStreamError("send STREAM_OPEN failed");
    }

    LOG_INFO << "[RpcChannel] stream opened, streamId=" << streamId
             << ", requestId=" << requestId << ", service=" << serviceName
             << ", method=" << methodName;

    return StreamHandle{streamId, requestId, true, ""};
}

bool RpcChannel::cancelStream(std::uint32_t streamId, std::string reason) {
    auto session = streamManager_.findStream(streamId);
    if (!session) {
        return false;
    }

    const std::uint64_t requestId = session->requestId();

    (void)session->markCancelled(reason);
    (void)streamManager_.removeStream(streamId);

    auto callbacks = takeCallbacks(streamId);

    const bool sent = sendStreamCancelMessage(streamId, requestId, reason);

    if (callbacks && callbacks->onError) {
        callbacks->onError(streamId, meta::RPC_CANCELLED, reason);
    }

    LOG_INFO << "[RpcChannel] stream cancelled, streamId=" << streamId
             << ", reason=" << reason;

    return sent;
}

void RpcChannel::onMessage(const TcpConnectionPtr& connection,
                           novanet::net::Buffer* buffer) {
    if (!connection || buffer == nullptr) {
        return;
    }

    while (true) {
        RpcMessage message;
        const RpcCodec::DecodeStatus status = codec_.tryDecode(*buffer, message);

        if (status == RpcCodec::DecodeStatus::kNeedMore) {
            break;
        }

        if (status == RpcCodec::DecodeStatus::kInvalid ||
            status != RpcCodec::DecodeStatus::kOk || !message.valid()) {
            LOG_ERROR << "[RpcChannel] decode error";
            onConnectionClosed("decode error");
            if (connection->connected()) {
                connection->shutdown();
            }
            return;
        }

        handleRpcMessage(message);
    }
}

void RpcChannel::onConnectionClosed(std::string reason) {
    const bool alreadyClosed =
        connectionClosed_.exchange(true, std::memory_order_acq_rel);
    if (alreadyClosed) {
        return;
    }

    stopTimers();

    static_cast<void>(pendingCalls_.failAll(reason));
    static_cast<void>(streamManager_.cancelAll(reason));

    std::vector<std::pair<std::uint32_t, StreamCallbacks>> callbacks;

    {
        std::lock_guard<std::mutex> lock(callbacksMutex_);
        callbacks.reserve(callbacks_.size());

        for (auto& item : callbacks_) {
            callbacks.emplace_back(item.first, std::move(item.second));
        }

        callbacks_.clear();
    }

    for (auto& item : callbacks) {
        if (item.second.onError) {
            item.second.onError(item.first, meta::RPC_CONNECTION_CLOSED, reason);
        }
    }

    LOG_WARN << "[RpcChannel] connection closed, reason=" << reason;
}

void RpcChannel::startTimers() {
    if (!connection_) {
        return;
    }

    auto* loop = connection_->getLoop();
    if (loop == nullptr) {
        return;
    }

    heartbeatPingTimer_ = loop->runEvery(options_.heartbeatIntervalSeconds,
                                         [this]() { (void)this->sendHeartbeatPing(); });

    heartbeatCheckTimer_ =
        loop->runEvery(options_.heartbeatCheckIntervalSeconds,
                       [this]() { (void)this->checkHeartbeatTimeout(); });

    streamTimeoutTimer_ = loop->runEvery(options_.streamTimeoutScanIntervalSeconds,
                                         [this]() { this->checkStreamTimeouts(); });

    LOG_INFO << "[RpcChannel] timers started";
}

void RpcChannel::stopTimers() {
    if (!connection_) {
        return;
    }

    auto* loop = connection_->getLoop();
    if (loop == nullptr) {
        return;
    }

    if (heartbeatPingTimer_.valid()) {
        loop->cancel(heartbeatPingTimer_);
    }

    if (heartbeatCheckTimer_.valid()) {
        loop->cancel(heartbeatCheckTimer_);
    }

    if (streamTimeoutTimer_.valid()) {
        loop->cancel(streamTimeoutTimer_);
    }
}

bool RpcChannel::sendHeartbeatPing() {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return false;
    }

    const Timestamp now = Timestamp::now();

    meta::HeartbeatMeta heartbeat;
    heartbeat.set_unix_millis(now.microSecondsSinceEpoch() / 1000);
    heartbeat.set_node_id(options_.nodeId);

    std::string payload;
    if (!heartbeat.SerializeToString(&payload)) {
        return false;
    }

    const std::uint64_t requestId = nextRequestId();

    RpcMessage ping(FrameType::HEARTBEAT_PING, 0, requestId, std::move(payload));
    if (!ping.valid()) {
        return false;
    }

    lastPingMicros_.store(now.microSecondsSinceEpoch(), std::memory_order_release);

    return sendRpcMessage(std::move(ping));
}

bool RpcChannel::checkHeartbeatTimeout() {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return true;
    }

    const std::int64_t lastPong = lastPongMicros_.load(std::memory_order_acquire);

    const Timestamp now = Timestamp::now();
    const double elapsed = timeDifference(now, Timestamp(lastPong));

    if (elapsed <= options_.heartbeatTimeoutSeconds) {
        return false;
    }

    LOG_WARN << "[RpcChannel] heartbeat timeout, elapsed=" << elapsed;

    onConnectionClosed("heartbeat timeout");

    if (connection_ && connection_->connected()) {
        connection_->shutdown();
    }

    return true;
}

void RpcChannel::checkStreamTimeouts() {
    const Timestamp now = Timestamp::now();

    auto expiredStreamIds = streamManager_.timeoutStreams(
        now, options_.streamIdleTimeoutSeconds, "client stream idle timeout");

    for (const auto streamId : expiredStreamIds) {
        auto callbacks = takeCallbacks(streamId);
        if (callbacks && callbacks->onError) {
            callbacks->onError(streamId, meta::RPC_TIMEOUT,
                               "client stream idle timeout");
        }

        LOG_WARN << "[RpcChannel] stream timeout, streamId=" << streamId;
    }
}

std::uint64_t RpcChannel::nextRequestId() {
    std::uint64_t id = nextRequestId_.fetch_add(1, std::memory_order_acq_rel);
    if (id == 0) {
        id = nextRequestId_.fetch_add(1, std::memory_order_acq_rel);
    }
    return id;
}

std::uint32_t RpcChannel::nextStreamId() {
    std::uint32_t id = nextStreamId_.fetch_add(1, std::memory_order_acq_rel);
    if (id == 0) {
        id = nextStreamId_.fetch_add(1, std::memory_order_acq_rel);
    }
    return id;
}

bool RpcChannel::sendRpcMessage(RpcMessage message) {
    if (connectionClosed_.load(std::memory_order_acquire)) {
        return false;
    }

    if (!connection_ || !connection_->connected()) {
        onConnectionClosed("connection closed before send");
        return false;
    }

    auto connection = connection_;
    auto* loop = connection->getLoop();
    if (loop == nullptr) {
        onConnectionClosed("connection loop is null");
        return false;
    }

    loop->queueInLoop([connection, message = std::move(message),
                       highWater = options_.sendHighWaterMarkBytes]() mutable {
        if (!connection || !connection->connected()) {
            return;
        }

        if (highWater != 0 && connection->outputBufferSize() >= highWater) {
            LOG_WARN << "[RpcChannel] send high water, shutdown connection";
            connection->shutdown();
            return;
        }

        RpcCodec codec;
        novanet::net::Buffer out;

        if (!codec.encode(message, out)) {
            LOG_ERROR << "[RpcChannel] encode failed";
            connection->shutdown();
            return;
        }

        connection->send(out.retrieveAllAsString());
    });

    return true;
}

bool RpcChannel::sendStreamOpenMessage(std::uint32_t streamId, std::uint64_t requestId,
                                       const std::string& serviceName,
                                       const std::string& methodName,
                                       const std::string& requestPayload) {
    meta::StreamOpenMeta openMeta;
    openMeta.set_service_name(serviceName);
    openMeta.set_method_name(methodName);
    openMeta.set_request_payload(requestPayload);

    std::string payload;
    if (!openMeta.SerializeToString(&payload)) {
        return false;
    }

    auto frame = StreamFrame::makeOpen(streamId, requestId, std::move(payload));
    if (!frame.valid()) {
        return false;
    }

    return sendRpcMessage(frame.releaseMessage());
}

bool RpcChannel::sendStreamCancelMessage(std::uint32_t streamId, std::uint64_t requestId,
                                         std::string reason) {
    meta::StreamCancelMeta cancelMeta;
    cancelMeta.set_reason(std::move(reason));

    std::string payload;
    if (!cancelMeta.SerializeToString(&payload)) {
        return false;
    }

    auto frame = StreamFrame::makeCancel(streamId, requestId, std::move(payload));
    if (!frame.valid()) {
        return false;
    }

    return sendRpcMessage(frame.releaseMessage());
}

bool RpcChannel::sendHeartbeatPong(const RpcMessage& ping) {
    RpcMessage pong(FrameType::HEARTBEAT_PONG, 0, ping.requestId(), ping.payload());
    if (!pong.valid()) {
        return false;
    }

    return sendRpcMessage(std::move(pong));
}

void RpcChannel::handleRpcMessage(const RpcMessage& message) {
    switch (message.frameType()) {
        case FrameType::UNARY_RESPONSE:
            handleUnaryResponse(message);
            break;
        case FrameType::STREAM_DATA:
            handleStreamData(message);
            break;
        case FrameType::STREAM_END:
            handleStreamEnd(message);
            break;
        case FrameType::STREAM_CANCEL:
            handleStreamCancel(message);
            break;
        case FrameType::ERROR_FRAME:
            handleErrorFrame(message);
            break;
        case FrameType::HEARTBEAT_PING:
            handleHeartbeatPing(message);
            break;
        case FrameType::HEARTBEAT_PONG:
            handleHeartbeatPong(message);
            break;
        default:
            LOG_WARN << "[RpcChannel] unexpected frame type";
            break;
    }
}

void RpcChannel::handleUnaryResponse(const RpcMessage& message) {
    meta::UnaryResponseMeta responseMeta;

    if (!responseMeta.ParseFromString(message.payload())) {
        static_cast<void>(pendingCalls_.fail(message.requestId(),
                                            "parse UnaryResponseMeta failed"));
        return;
    }

    if (responseMeta.error_code() != meta::RPC_OK) {
        static_cast<void>(pendingCalls_.fail(message.requestId(),
                                            responseMeta.error_text()));
        return;
    }

    static_cast<void>(pendingCalls_.complete(message.requestId(),
                                            responseMeta.response_payload()));
}

void RpcChannel::handleStreamData(const RpcMessage& message) {
    auto session = streamManager_.findStream(message.streamId());
    if (!session || !session->canReceiveData()) {
        LOG_WARN << "[RpcChannel] drop late STREAM_DATA, streamId=" << message.streamId();
        return;
    }

    const auto result = streamManager_.handleDataFrame(message);
    if (result != StreamManager::Result::kOk) {
        failStream(message.streamId(), meta::RPC_STREAM_PROTOCOL_ERROR,
                   "handle STREAM_DATA failed");
        return;
    }

    meta::StreamDataMeta dataMeta;
    if (!dataMeta.ParseFromString(message.payload())) {
        failStream(message.streamId(), meta::RPC_PARSE_RESPONSE_FAILED,
                   "parse StreamDataMeta failed");
        return;
    }

    chat::GenerateChunk chunk;
    if (!chunk.ParseFromString(dataMeta.data())) {
        failStream(message.streamId(), meta::RPC_PARSE_RESPONSE_FAILED,
                   "parse GenerateChunk failed");
        return;
    }

    auto callbacks = findCallbacks(message.streamId());
    if (callbacks && callbacks->onData) {
        callbacks->onData(message.streamId(), dataMeta.sequence(), chunk);
    }
}

void RpcChannel::handleStreamEnd(const RpcMessage& message) {
    meta::StreamEndMeta endMeta;
    if (!endMeta.ParseFromString(message.payload())) {
        failStream(message.streamId(), meta::RPC_PARSE_RESPONSE_FAILED,
                   "parse StreamEndMeta failed");
        return;
    }

    const auto result = streamManager_.handleEndFrame(message);
    if (result != StreamManager::Result::kOk) {
        failStream(message.streamId(), meta::RPC_STREAM_PROTOCOL_ERROR,
                   "handle STREAM_END failed");
        return;
    }

    (void)streamManager_.removeStream(message.streamId());

    auto callbacks = takeCallbacks(message.streamId());
    if (!callbacks) {
        return;
    }

    if (endMeta.error_code() == meta::RPC_OK) {
        if (callbacks->onEnd) {
            callbacks->onEnd(message.streamId(), meta::RPC_OK, "");
        }
        return;
    }

    if (callbacks->onError) {
        callbacks->onError(message.streamId(), endMeta.error_code(),
                           endMeta.error_text());
    }
}

void RpcChannel::handleStreamCancel(const RpcMessage& message) {
    meta::StreamCancelMeta cancelMeta;
    if (!cancelMeta.ParseFromString(message.payload())) {
        failStream(message.streamId(), meta::RPC_PARSE_RESPONSE_FAILED,
                   "parse StreamCancelMeta failed");
        return;
    }

    failStream(message.streamId(), meta::RPC_CANCELLED, cancelMeta.reason());
}

void RpcChannel::handleErrorFrame(const RpcMessage& message) {
    meta::ErrorFrameMeta errorMeta;
    if (!errorMeta.ParseFromString(message.payload())) {
        return;
    }

    if (message.streamId() != 0) {
        failStream(message.streamId(), errorMeta.error_code(), errorMeta.error_text());
        return;
    }

    if (message.requestId() != 0) {
        static_cast<void>(pendingCalls_.fail(message.requestId(),
                                            errorMeta.error_text()));
    }
}

void RpcChannel::handleHeartbeatPing(const RpcMessage& message) {
    (void)sendHeartbeatPong(message);
}

void RpcChannel::handleHeartbeatPong(const RpcMessage& message) {
    (void)message;
    lastPongMicros_.store(Timestamp::now().microSecondsSinceEpoch(),
                          std::memory_order_release);
}

void RpcChannel::saveCallbacks(std::uint32_t streamId, StreamCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    callbacks_[streamId] = std::move(callbacks);
}

void RpcChannel::eraseCallbacks(std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    callbacks_.erase(streamId);
}

std::optional<RpcChannel::StreamCallbacks> RpcChannel::takeCallbacks(
    std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(callbacksMutex_);

    auto it = callbacks_.find(streamId);
    if (it == callbacks_.end()) {
        return std::nullopt;
    }

    StreamCallbacks callbacks = std::move(it->second);
    callbacks_.erase(it);
    return callbacks;
}

std::optional<RpcChannel::StreamCallbacks> RpcChannel::findCallbacks(
    std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(callbacksMutex_);

    auto it = callbacks_.find(streamId);
    if (it == callbacks_.end()) {
        return std::nullopt;
    }

    return it->second;
}

void RpcChannel::failStream(std::uint32_t streamId, meta::RpcErrorCode errorCode,
                            std::string errorText) {
    auto session = streamManager_.findStream(streamId);
    if (session) {
        (void)session->markCancelled(errorText);
    }

    (void)streamManager_.removeStream(streamId);

    auto callbacks = takeCallbacks(streamId);
    if (callbacks && callbacks->onError) {
        callbacks->onError(streamId, errorCode, std::move(errorText));
    }
}

RpcChannel::StreamHandle RpcChannel::makeStreamError(std::string errorText) {
    return StreamHandle{0, 0, false, std::move(errorText)};
}

}  // namespace novanet::rpc
