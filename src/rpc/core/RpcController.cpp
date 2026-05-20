#include "novanet/rpc/core/RpcController.h"

#include <string>
#include <utility>

namespace novanet::rpc {

void RpcController::Reset() {
    failed_ = false;
    timeout_ = false;
    canceled_ = false;
    errorText_.clear();
    cancelCallbacks_.clear();
}

bool RpcController::Failed() const {
    return failed_;
}

std::string RpcController::ErrorText() const {
    return errorText_;
}

void RpcController::StartCancel() {
    if (canceled_) {
        return;
    }
    canceled_ = true;
    runCancelCallbacks();
}

void RpcController::SetFailed(const std::string &reason) {
    setFailed(reason);
}

bool RpcController::IsCanceled() const {
    return canceled_;
}

void RpcController::NotifyOnCancel(google::protobuf::Closure *callback) {
    if (callback == nullptr) {
        return;
    }
    if (canceled_) {
        callback->Run();
        return;
    }
    cancelCallbacks_.push_back(callback);
}

void RpcController::reset() {
    Reset();
}

bool RpcController::failed() const noexcept {
    return failed_;
}

bool RpcController::timeout() const noexcept {
    return timeout_;
}

bool RpcController::canceled() const noexcept {
    return canceled_;
}

const std::string &RpcController::errorText() const noexcept {
    return errorText_;
}

void RpcController::setFailed(std::string reason) {
    failed_ = true;
    errorText_ = std::move(reason);
}

void RpcController::setTimeout(bool on) {
    if (on) {
        timeout_ = true;
        failed_ = true;
        if (errorText_.empty()) {
            errorText_ = "rpc timeout";
        }
        return;
    }
    timeout_ = false;
    if (errorText_ == "rpc timeout") {
        failed_ = false;
        errorText_.clear();
    }
}

void RpcController::setTimeout(bool on, std::string reason) {
    if (on) {
        timeout_ = true;
        failed_ = true;
        if (reason.empty()) {
            errorText_ = "rpc timeout";
        } else {
            errorText_ = std::move(reason);
        }
        return;
    }
    setTimeout(false);
}

void RpcController::runCancelCallbacks() {
    auto callbacks = std::move(cancelCallbacks_);
    cancelCallbacks_.clear();
    for (auto *callback : callbacks) {
        if (callback != nullptr) {
            callback->Run();
        }
    }
}
}  // namespace novanet::rpc