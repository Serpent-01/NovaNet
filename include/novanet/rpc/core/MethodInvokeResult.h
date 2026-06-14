#pragma once

#include <string>
#include <utility>

#include "novanet/rpc/core/RpcStatus.h"

namespace novanet::rpc {

class MethodInvokeResult final {
public:
    MethodInvokeResult() = default;
    static MethodInvokeResult success(std::string responseBytes) {
        MethodInvokeResult result;
        result.status_ = RpcStatus::success();
        result.responseBytes_ = std::move(responseBytes);
        return result;
    }

    static MethodInvokeResult failure(RpcErrorCode code,
                                      std::string errorText) {
        MethodInvokeResult result;
        result.status_ = RpcStatus::failure(code, std::move(errorText));
        result.responseBytes_.clear();
        return result;
    }

    static MethodInvokeResult failure(RpcStatus status) {
        MethodInvokeResult result;

        if (status.ok()) {
            status = RpcStatus::failure(
                RPC_UNKNOWN_ERROR,
                "MethodInvokeResult::failure received success status");
        }

        result.status_ = std::move(status);
        result.responseBytes_.clear();
        return result;
    }

    [[nodiscard]] bool ok() const noexcept {
        return status_.ok();
    }

    [[nodiscard]] bool failed() const noexcept {
        return status_.failed();
    }

    [[nodiscard]] const RpcStatus& status() const noexcept {
        return status_;
    }

    [[nodiscard]] RpcErrorCode errorCode() const noexcept {
        return status_.errorCode();
    }

    [[nodiscard]] const std::string& errorText() const noexcept {
        return status_.errorText();
    }

    [[nodiscard]] const std::string& responseBytes() const noexcept {
        return responseBytes_;
    }

    /*
     * 用于把 responseBytes 移入 UnaryResponseMeta，
     * 避免一次不必要的拷贝。
     */
    std::string releaseResponseBytes() noexcept {
        return std::move(responseBytes_);
    }

private:
    RpcStatus status_{RpcStatus::success()};
    std::string responseBytes_;
};
}  // namespace novanet::rpc