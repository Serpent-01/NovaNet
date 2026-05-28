#include "novanet/rpc/core/PendingCallManager.h"

#include <memory>
#include <mutex>
#include <utility>

#include "novanet/rpc/core/PendingCall.h"

namespace novanet::rpc {

PendingCallManager::~PendingCallManager() {
    static_cast<void>(failAll("pending call manager destroyed"));
}

std::shared_ptr<PendingCall> PendingCallManager::create(
    std::uint64_t requestId) {
    if (!validRequestId(requestId)) {
        return nullptr;
    }
    auto call = std::make_shared<PendingCall>(requestId);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, inserted] = pending_.emplace(requestId, call);
        if (!inserted) {
            return nullptr;
        }
    }
    return call;
}
bool PendingCallManager::add(std::uint64_t requestId,
                             std::shared_ptr<PendingCall> call) {
    if (!validRequestId(requestId)) {
        return false;
    }

    if (!call) {
        return false;
    }

    if (call->requestId() != requestId) {
        return false;
    }

    if (!call->pending()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto [it, inserted] = pending_.emplace(requestId, std::move(call));
    return inserted;
}

std::shared_ptr<PendingCall> PendingCallManager::find(
    std::uint64_t requestId) const {
    if (!validRequestId(requestId)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(requestId);
    if (it == pending_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<PendingCall> PendingCallManager::remove(
    std::uint64_t requestId) {
    if (!validRequestId(requestId)) {
        return nullptr;
    }

    std::shared_ptr<PendingCall> call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(requestId);
        if (it == pending_.end()) {
            return nullptr;
        }
        call = std::move(it->second);
        pending_.erase(it);
    }
    return call;
}
PendingCallManager::FinishResult PendingCallManager::complete(
    std::uint64_t requestId, std::string responseBytes) {
    auto call = remove(requestId);
    if (!call) {
        return FinishResult::kNotFound;
    }
    if (!call->markDone(std::move(responseBytes))) {
        return FinishResult::kAlreadyFinished;
    }
    return FinishResult::kCompleted;
}

PendingCallManager::FinishResult PendingCallManager::fail(
    std::uint64_t requestId, std::string errorText) {
    auto call = remove(requestId);
    if (!call) {
        return FinishResult::kNotFound;
    }
    if (!call->markFailed(std::move(errorText))) {
        return FinishResult::kAlreadyFinished;
    }
    return FinishResult::kCompleted;
}

PendingCallManager::FinishResult PendingCallManager::timeout(
    std::uint64_t requestId, std::string errorText) {
    auto call = remove(requestId);
    if (!call) {
        return FinishResult::kNotFound;
    }
    if (!call->markTimeout(errorText)) {
        return FinishResult::kAlreadyFinished;
    }
    return FinishResult::kCompleted;
}

std::size_t PendingCallManager::failAll(std::string errorText) {
    std::vector<std::shared_ptr<PendingCall>> calls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calls.reserve(pending_.size());
        for (auto& entry : pending_) {
            calls.push_back(std::move(entry.second));
        }
        pending_.clear();
    }
    std::size_t failedCount = 0;
    for (auto& call : calls) {
        if (!call) {
            continue;
        }
        if (call->markFailed(errorText)) {
            ++failedCount;
        }
    }
    return failedCount;
}

std::size_t PendingCallManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

bool PendingCallManager::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.empty();
}

std::vector<std::uint64_t> PendingCallManager::activeRequestIds() const {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.reserve(pending_.size());
        for (const auto& entry : pending_) {
            ids.push_back(entry.first);
        }
    }
    return ids;
}

bool PendingCallManager::validRequestId(std::uint64_t requestId) noexcept {
    return requestId != 0;
}
}  // namespace novanet::rpc