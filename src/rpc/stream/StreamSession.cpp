#include "novanet/rpc/stream/StreamSession.h"

#include <utility>

namespace novanet::rpc {

StreamSession::StreamSession(std::uint32_t streamId, std::uint64_t requestId,
                             std::string serviceName, std::string methodName)
    : streamId_(streamId),
      requestId_(requestId),
      serviceName_(std::move(serviceName)),
      methodName_(std::move(methodName)),
      lastActiveTime_(novanet::base::Timestamp::now()) {
}

std::uint32_t StreamSession::streamId() const noexcept {
    return streamId_;
}

std::uint64_t StreamSession::requestId() const noexcept {
    return requestId_;
}

std::string StreamSession::serviceName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serviceName_;
}

std::string StreamSession::methodName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return methodName_;
}

StreamSession::State StreamSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string StreamSession::cancelReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelReason_;
}

bool StreamSession::open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kOpen;
}

bool StreamSession::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kClosed;
}

bool StreamSession::cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kCancelled;
}

bool StreamSession::terminal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isTerminal(state_);
}

bool StreamSession::localEndSent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return localEndSent_;
}

bool StreamSession::remoteEndReceived() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteEndReceived_;
}

bool StreamSession::canSendData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return canSendInState(state_);
}

bool StreamSession::canReceiveData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return canReceiveInState(state_);
}

bool StreamSession::markLocalEnd() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isTerminal(state_)) {
        return false;
    }

    if (localEndSent_) {
        return false;
    }

    localEndSent_ = true;
    lastActiveTime_ = novanet::base::Timestamp::now();

    if (state_ == State::kOpen) {
        state_ = State::kHalfClosedLocal;
        return true;
    }

    if (state_ == State::kHalfClosedRemote) {
        state_ = State::kClosed;
        return true;
    }

    return false;
}

bool StreamSession::markRemoteEnd() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isTerminal(state_)) {
        return false;
    }

    if (remoteEndReceived_) {
        return false;
    }

    remoteEndReceived_ = true;
    lastActiveTime_ = novanet::base::Timestamp::now();

    if (state_ == State::kOpen) {
        state_ = State::kHalfClosedRemote;
        return true;
    }

    if (state_ == State::kHalfClosedLocal) {
        state_ = State::kClosed;
        return true;
    }

    return false;
}

bool StreamSession::markCancelled(std::string reason) {
    ErrorCallback cb;
    std::string errorText;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (isTerminal(state_)) {
            return false;
        }

        state_ = State::kCancelled;
        cancelReason_ = std::move(reason);
        lastActiveTime_ = novanet::base::Timestamp::now();

        errorText = cancelReason_;
        cb = onError_;
    }

    if (cb) {
        cb(streamId_, errorText);
    }

    return true;
}

bool StreamSession::markTimeout(std::string reason) {
    if (reason.empty()) {
        reason = "stream timeout";
    }

    return markCancelled(std::move(reason));
}

void StreamSession::touch() {
    touch(novanet::base::Timestamp::now());
}

void StreamSession::touch(novanet::base::Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isTerminal(state_)) {
        return;
    }

    lastActiveTime_ = now;
}

novanet::base::Timestamp StreamSession::lastActiveTime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastActiveTime_;
}

double StreamSession::idleSeconds(novanet::base::Timestamp now) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!lastActiveTime_.valid() || !now.valid()) {
        return 0.0;
    }

    return novanet::base::timeDifference(now, lastActiveTime_);
}

bool StreamSession::expired(novanet::base::Timestamp now,
                            double timeoutSeconds) const {
    if (timeoutSeconds <= 0.0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (isTerminal(state_)) {
        return false;
    }

    if (!lastActiveTime_.valid() || !now.valid()) {
        return false;
    }

    return novanet::base::timeDifference(now, lastActiveTime_) >=
           timeoutSeconds;
}

void StreamSession::setDataCallback(DataCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onData_ = std::move(cb);
}

void StreamSession::setEndCallback(EndCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onEnd_ = std::move(cb);
}

void StreamSession::setErrorCallback(ErrorCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onError_ = std::move(cb);
}

bool StreamSession::notifyData(const std::string& payload) {
    DataCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!canReceiveInState(state_)) {
            return false;
        }

        lastActiveTime_ = novanet::base::Timestamp::now();
        cb = onData_;
    }

    if (cb) {
        cb(streamId_, payload);
    }

    return true;
}

bool StreamSession::notifyEnd() {
    EndCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (isTerminal(state_)) {
            return false;
        }

        if (remoteEndReceived_) {
            return false;
        }

        remoteEndReceived_ = true;
        lastActiveTime_ = novanet::base::Timestamp::now();

        if (state_ == State::kOpen) {
            state_ = State::kHalfClosedRemote;
        } else if (state_ == State::kHalfClosedLocal) {
            state_ = State::kClosed;
        } else {
            return false;
        }

        cb = onEnd_;
    }

    if (cb) {
        cb(streamId_);
    }

    return true;
}

void StreamSession::notifyError(const std::string& errorText) {
    ErrorCallback cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = onError_;
    }

    if (cb) {
        cb(streamId_, errorText);
    }
}

bool StreamSession::canSendInState(State state) noexcept {
    return state == State::kOpen || state == State::kHalfClosedRemote;
}

bool StreamSession::canReceiveInState(State state) noexcept {
    return state == State::kOpen || state == State::kHalfClosedLocal;
}

bool StreamSession::isTerminal(State state) noexcept {
    return state == State::kClosed || state == State::kCancelled;
}

std::string_view StreamSession::stateToString(State state) noexcept {
    switch (state) {
        case State::kOpen:
            return "kOpen";

        case State::kHalfClosedLocal:
            return "kHalfClosedLocal";

        case State::kHalfClosedRemote:
            return "kHalfClosedRemote";

        case State::kClosed:
            return "kClosed";

        case State::kCancelled:
            return "kCancelled";

        default:
            return "UnknownStreamSessionState";
    }
}

}  // namespace novanet::rpc