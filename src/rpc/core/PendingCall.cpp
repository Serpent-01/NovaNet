#include "novanet/rpc/core/PendingCall.h"

#include <chrono>
#include <mutex>
#include <utility>

namespace novanet::rpc {

PendingCall::PendingCall(std::uint64_t requestId) noexcept
    : requestId_(requestId) {
}

std::uint64_t PendingCall::requestId() const noexcept {
    return requestId_;
}

PendingCall::State PendingCall::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
std::string PendingCall::responseBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return responseBytes_;
}
bool PendingCall::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPending;
}

bool PendingCall::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kDone;
}

bool PendingCall::timeout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kTimeout;
}
bool PendingCall::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kFailed;
}
bool PendingCall::completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isTerminal(state_);
}

std::string PendingCall::errorText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorText_;
}

bool PendingCall::markDone(std::string responseBytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kPending) {
            return false;
        }
        responseBytes_ = std::move(responseBytes);
        errorText_.clear();
        state_ = State::kDone;
    }
    cond_.notify_all();
    return true;
}

bool PendingCall::markTimeout(std::string errorText) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kPending) {
            return false;
        }
        responseBytes_.clear();
        errorText_ = std::move(errorText);
        state_ = State::kTimeout;
    }
    cond_.notify_all();
    return true;
}

bool PendingCall::markFailed(std::string errorText) {
    {
        std::lock_guard<std::mutex> mutex(mutex_);
        if (state_ != State::kPending) {
            return false;
        }
        responseBytes_.clear();
        errorText_ = std::move(errorText);
        state_ = State::kFailed;
    }
    cond_.notify_all();
    return true;
}

PendingCall::State PendingCall::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return isTerminal(state_); });
    return state_;
}

PendingCall::State PendingCall::waitFor(std::chrono::milliseconds duration) {
    if (duration <= std::chrono::milliseconds::zero()) {
        static_cast<void>(markTimeout("rpc call timeout"));
        return state();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool finished =
        cond_.wait_for(lock, duration, [this] { return isTerminal(state_); });
    if (finished) {
        return state_;
    }
    /*
     * 走到这里说明 wait_for 超时。
     *
     * 但仍然要检查 state_ 是否是 kPending：
     * 可能刚好有另一个线程在超时边界附近 markDone/markFailed。
     */
    if (state_ == State::kPending) {
        responseBytes_.clear();
        errorText_ = "rpc call timeout";
        state_ = State::kTimeout;
        lock.unlock();
        cond_.notify_all();
        return State::kTimeout;
    }
    return state_;
}

std::string_view PendingCall::stateToString(State state) noexcept {
    switch (state) {
        case State::kPending:
            return "kPending";
        case State::kDone:
            return "kDone";
        case State::kTimeout:
            return "kTimeout";
        case State::kFailed:
            return "kFailed";
        default:
            return "UnknownPendingCallState";
    }
}

bool PendingCall::isTerminal(State state) noexcept {
    return state == State::kDone || state == State::kTimeout ||
           state == State::kFailed;
}

}  // namespace novanet::rpc