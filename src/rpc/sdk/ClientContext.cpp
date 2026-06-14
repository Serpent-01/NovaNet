#include "novanet/rpc/sdk/ClientContext.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "novanet/base/Timestamp.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc::sdk {

using novanet::base::addTime;
using novanet::base::timeDifference;
using novanet::base::Timestamp;
namespace meta = novanet::rpc::meta;

ClientContext::ClientContext() = default;

void ClientContext::setTimeoutSeconds(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (seconds <= 0.0) {
        hasDeadline_ = false;
        deadline_ = Timestamp::invalid();
        return;
    }

    hasDeadline_ = true;
    deadline_ = addTime(Timestamp::now(), seconds);
}

void ClientContext::setDeadline(Timestamp deadline) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!deadline.valid()) {
        hasDeadline_ = false;
        deadline_ = Timestamp::invalid();
        return;
    }
    hasDeadline_ = true;
    deadline_ = deadline;
}

void ClientContext::clearDeadline() {
    std::lock_guard<std::mutex> lock(mutex_);
    hasDeadline_ = false;
    deadline_ = Timestamp::invalid();
}

bool ClientContext::hasDeadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasDeadline_;
}

Timestamp ClientContext::deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deadline_;
}

double ClientContext::remainingTimeoutSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!hasDeadline_ || !deadline_.valid()) {
        return -1.0;
    }

    const Timestamp now = Timestamp::now();
    const double remaining = timeDifference(deadline_, now);
    if (remaining <= 0.0) {
        return 0.0;
    }
    return remaining;
}

bool ClientContext::expired() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!hasDeadline_ || !deadline_.valid()) {
        return false;
    }

    return Timestamp::now() >= deadline_;
}

void ClientContext::setMetadata(std::string key, std::string value) {
    if (key.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    metadata_[std::move(key)] = std::move(value);
}

std::optional<std::string> ClientContext::findMetadata(
    const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metadata_.find(key);
    if (it == metadata_.end()) {
        return nullptr;
    }
    return it->second;
}

ClientContext::MetadataMap ClientContext::metadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_;
}

void ClientContext::removeMetadata(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_.erase(key);
}

void ClientContext::clearMetadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_.clear();
}

void ClientContext::cancel(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reason.empty()) {
            reason = "client cancelled";
        }

        cancelReason_ = std::move(reason);
    }
    cancelled_.store(true, std::memory_order_release);
}

bool ClientContext::cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
}

std::string ClientContext::cancelReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelReason_;
}

void ClientContext::setRequestId(std::uint64_t requestId) noexcept {
    requestId_.store(requestId, std::memory_order_release);
}

std::uint64_t ClientContext::requestId() const noexcept {
    return requestId_.load(std::memory_order_acquire);
}

void ClientContext::setStreamId(std::uint32_t streamId) noexcept {
    streamId_.store(streamId, std::memory_order_release);
}

std::uint32_t ClientContext::streamId() const noexcept {
    return streamId_.load(std::memory_order_acquire);
}

void ClientContext::setError(meta::RpcErrorCode errorCode, std::string errorText) {
    std::lock_guard<std::mutex> lock(mutex_);

    errorCode_ = errorCode;
    errorText_ = std::move(errorText);
}

void ClientContext::clearError() {
    std::lock_guard<std::mutex> lock(mutex_);

    errorCode_ = meta::RPC_OK;
    errorText_.clear();
}

bool ClientContext::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorCode_ != meta::RPC_OK;
}

meta::RpcErrorCode ClientContext::errorCode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorCode_;
}

std::string ClientContext::errorText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorText_;
}

void ClientContext::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hasDeadline_ = false;
        deadline_ = Timestamp::invalid();

        metadata_.clear();

        cancelReason_.clear();
        errorCode_ = meta::RPC_OK;
        errorText_.clear();
    }
    cancelled_.store(false, std::memory_order_release);
    requestId_.store(0, std::memory_order_release);
    streamId_.store(0, std::memory_order_release);
}

}  // namespace novanet::rpc::sdk