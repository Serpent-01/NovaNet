#include "novanet/rpc/stream/StreamManager.h"

#include <mutex>
#include <utility>

#include "novanet/rpc/stream/StreamFrame.h"
#include "novanet/rpc/stream/StreamSession.h"

namespace novanet::rpc {
StreamManager::SessionPtr
StreamManager::createStream(std::uint32_t streamId, std::uint64_t requestId,
                            std::string serviceName, std::string methodName,
                            DataCallback onData, EndCallback onEnd,
                            ErrorCallback onError) {
    if (!validStreamId(streamId)) {
        return nullptr;
    }
    if (!validRequestId(requestId)) {
        return nullptr;
    }
    if (serviceName.empty() || methodName.empty()) {
        return nullptr;
    }
    auto session = std::make_shared<StreamSession>(
        streamId, requestId, std::move(serviceName), std::move(methodName));
    if (onData) {
        session->setDataCallback(std::move(onData));
    }
    if (onEnd) {
        session->setEndCallback(std::move(onEnd));
    }
    if (onError) {
        session->setErrorCallback(std::move(onError));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, inserted] = streams_.emplace(streamId, session);
        if (!inserted) {
            return nullptr;
        }
    }

    return session;
}

bool StreamManager::addStream(SessionPtr session) {
    if (!session) {
        return false;
    }

    if (!validStreamId(session->streamId())) {
        return false;
    }

    if (!validRequestId(session->requestId())) {
        return false;
    }

    if (session->terminal()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto [it, inserted] =
        streams_.emplace(session->streamId(), std::move(session));
    return inserted;
}

StreamManager::SessionPtr
StreamManager::findStream(std::uint32_t streamId) const {
    if (!validStreamId(streamId)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return nullptr;
    }
    return it->second;
}

StreamManager::SessionPtr StreamManager::removeStream(std::uint32_t streamId) {
    if (!validStreamId(streamId)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return nullptr;
    }
    SessionPtr session = std::move(it->second);
    streams_.erase(it);
    return session;
}

StreamManager::Result StreamManager::markLocalEnd(std::uint32_t streamId) {
    auto session = findStream(streamId);
    if (!session) {
        return Result::kStreamNotFound;
    }
    if (!session->markLocalEnd()) {
        return Result::kStateRejected;
    }
    if (session->terminal()) {
        static_cast<void>(removeStreamIfSame(streamId, session));
    }
    return Result::kOk;
}
StreamManager::Result StreamManager::handleDataFrame(const RpcMessage& msg) {
    StreamFrame frame(msg);
    if (!frame.valid() || frame.isData()) {
        return Result::kInvalidFrame;
    }
    auto session = findStream(frame.streamId());
    if (!session) {
        return Result::kStreamNotFound;
    }

    /*
     * notifyData 内部会检查 canReceiveData()。
     * 如果 END/CANCEL 后又收到 DATA，会返回 false。
     */
    if (!session->notifyData(frame.payload())) {
        return Result::kStateRejected;
    }
    return Result::kOk;
}

StreamManager::Result StreamManager::handleEndFrame(const RpcMessage& msg) {
    StreamFrame frame(msg);
    if (!frame.valid() || frame.isEnd()) {
        return Result::kInvalidFrame;
    }
    auto session = findStream(frame.streamId());
    if (!session) {
        return Result::kStreamNotFound;
    }

    if (!session->notifyEnd()) {
        return Result::kStateRejected;
    }

    /*
     * 如果双方都 END，session 会进入 kClosed。
     * 这时可以从 manager 中移除。
     *
     * 如果当前只是 kHalfClosedRemote，说明 full-duplex 语义下本端还没 END，
     * manager 先保留 session。
     *
     * server streaming 场景下，如果本端在 open 后已经 markLocalEnd，
     * 那收到 remote END 后会进入 kClosed，并在这里被移除。
     */
    if (session->terminal()) {
        static_cast<void>(removeStreamIfSame(frame.streamId(), session));
    }
    return Result::kOk;
}

StreamManager::Result StreamManager::handleCancelFrame(const RpcMessage& msg) {
    StreamFrame frame(msg);
    if (!frame.valid() || !frame.isCancel()) {
        return Result::kInvalidFrame;
    }
    auto session = removeStream(frame.streamId());
    if (!session) {
        return Result::kStreamNotFound;
    }
    std::string reason = frame.payload();

    if (reason.empty()) {
        reason = "stream cancelled";
    }
    if (!session->markCancelled(std::move(reason))) {
        return Result::kStateRejected;
    }
    return Result::kOk;
}

std::size_t StreamManager::cancelAll(std::string reason) {
    std::vector<SessionPtr> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.reserve(sessions.size());
        for (auto& entry : streams_) {
            sessions.push_back(std::move(entry.second));
        }
        streams_.clear();
    }
    std::size_t cancelledCount = 0;
    for (auto& session : sessions) {
        if (!session) {
            continue;
        }
        if (session->markCancelled(reason)) {
            ++cancelledCount;
        }
    }
    return cancelledCount;
}
std::vector<std::uint32_t>
StreamManager::timeoutStreams(novanet::base::Timestamp now,
                              double timeoutSeconds, std::string reason) {
    std::vector<std::uint32_t> timeoutIds;
    if (!now.valid()) {
        return timeoutIds;
    }
    if (timeoutSeconds <= 0.0) {
        return timeoutIds;
    }
    if (reason.empty()) {
        reason = "stream idle timeout";
    }
    /*
     * 先做快照，不长时间持有 manager 锁。
     * 后面 expired()/markTimeout() 都可能访问 session 自己的锁。
     */
    std::vector<SessionPtr> sessions = snapshotSessions();
    for (auto& session : sessions) {
        if (!session) {
            continue;
        }

        if (!session->expired(now, timeoutSeconds)) {
            continue;
        }

        const std::uint32_t streamId = session->streamId();

        /*
         * 只有当前 map 中的 session 仍然是这个对象时才移除。
         * 防止并发情况下误删同 streamId 后来新建的 session。
         */
        if (!removeStreamIfSame(streamId, session)) {
            continue;
        }

        if (session->markTimeout(reason)) {
            timeoutIds.push_back(streamId);
        }
    }

    return timeoutIds;
}

std::size_t StreamManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

bool StreamManager::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.empty();
}

std::vector<std::uint32_t> StreamManager::activeStreamIds() const {
    std::vector<std::uint32_t> ids;

    std::lock_guard<std::mutex> lock(mutex_);

    ids.reserve(streams_.size());

    for (const auto& entry : streams_) {
        ids.push_back(entry.first);
    }

    return ids;
}

std::string_view StreamManager::resultToString(Result result) noexcept {
    switch (result) {
    case Result::kOk:
        return "kOk";

    case Result::kInvalidArgument:
        return "kInvalidArgument";

    case Result::kInvalidFrame:
        return "kInvalidFrame";

    case Result::kDuplicateStream:
        return "kDuplicateStream";

    case Result::kStreamNotFound:
        return "kStreamNotFound";

    case Result::kStateRejected:
        return "kStateRejected";

    default:
        return "UnknownStreamManagerResult";
    }
}

bool StreamManager::validStreamId(std::uint32_t streamId) noexcept {
    return streamId != 0;
}

bool StreamManager::validRequestId(std::uint64_t requestId) noexcept {
    return requestId != 0;
}

bool StreamManager::removeStreamIfSame(std::uint32_t streamId,
                                       const SessionPtr& expected) {
    if (!validStreamId(streamId)) {
        return false;
    }
    if (!expected) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return false;
    }

    if (it->second != expected) {
        return false;
    }

    streams_.erase(it);
    return true;
}

std::vector<StreamManager::SessionPtr> StreamManager::snapshotSessions() const {
    std::vector<SessionPtr> sessions;

    std::lock_guard<std::mutex> lock(mutex_);

    sessions.reserve(streams_.size());

    for (const auto& entry : streams_) {
        sessions.push_back(entry.second);
    }

    return sessions;
}

} // namespace novanet::rpc