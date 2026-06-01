#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "rpc_meta.pb.h"

namespace novanet::rpc {
using namespace meta;
class RpcStatus final {
public:
    RpcStatus() = default;

    static RpcStatus success() {
        return RpcStatus(meta::RPC_OK, "");
    }

    static RpcStatus failure(meta::RpcErrorCode code, std::string errorText) {
        if (code == meta::RPC_OK) {
            code = meta::RPC_UNKNOWN_ERROR;
        }

        if (errorText.empty()) {
            errorText = std::string(defaultErrorText(code));
        }

        return RpcStatus(code, std::move(errorText));
    }

    [[nodiscard]] bool ok() const noexcept {
        return errorCode_ == RPC_OK;
    }

    [[nodiscard]] bool failed() const noexcept {
        return !ok();
    }

    [[nodiscard]] RpcErrorCode errorCode() const noexcept {
        return errorCode_;
    }

    [[nodiscard]] const std::string& errorText() const noexcept {
        return errorText_;
    }

    [[nodiscard]] std::string toString() const {
        if (ok()) {
            return "RPC_OK";
        }

        std::string result;
        result.append(std::string(errorCodeToString(errorCode_)));

        if (!errorText_.empty()) {
            result.append(": ");
            result.append(errorText_);
        }

        return result;
    }

    [[nodiscard]] static std::string_view
    errorCodeToString(RpcErrorCode code) noexcept {
        switch (code) {
        case RPC_OK:
            return "RPC_OK";
        case RPC_UNKNOWN_ERROR:
            return "RPC_UNKNOWN_ERROR";
        case RPC_BAD_REQUEST:
            return "RPC_BAD_REQUEST";
        case RPC_INVALID_FRAME:
            return "RPC_INVALID_FRAME";
        case RPC_UNSUPPORTED_FRAME_TYPE:
            return "RPC_UNSUPPORTED_FRAME_TYPE";
        case RPC_SERVICE_NOT_FOUND:
            return "RPC_SERVICE_NOT_FOUND";
        case RPC_METHOD_NOT_FOUND:
            return "RPC_METHOD_NOT_FOUND";
        case RPC_PARSE_REQUEST_FAILED:
            return "RPC_PARSE_REQUEST_FAILED";
        case RPC_SERIALIZE_RESPONSE_FAILED:
            return "RPC_SERIALIZE_RESPONSE_FAILED";
        case RPC_INVOKE_FAILED:
            return "RPC_INVOKE_FAILED";
        case RPC_TIMEOUT:
            return "RPC_TIMEOUT";
        case RPC_CANCELLED:
            return "RPC_CANCELLED";
        case RPC_STREAM_NOT_FOUND:
            return "RPC_STREAM_NOT_FOUND";
        case RPC_STREAM_CLOSED:
            return "RPC_STREAM_CLOSED";
        case RPC_STREAM_CANCELLED:
            return "RPC_STREAM_CANCELLED";
        case RPC_BACKPRESSURE:
            return "RPC_BACKPRESSURE";
        case RPC_RESOURCE_EXHAUSTED:
            return "RPC_RESOURCE_EXHAUSTED";
        default:
            return "RPC_UNRECOGNIZED_ERROR";
        }
    }

    [[nodiscard]] static std::string_view
    defaultErrorText(RpcErrorCode code) noexcept {
        switch (code) {
        case RPC_OK:
            return "";
        case RPC_UNKNOWN_ERROR:
            return "unknown rpc error";
        case RPC_BAD_REQUEST:
            return "bad rpc request";
        case RPC_INVALID_FRAME:
            return "invalid rpc frame";
        case RPC_UNSUPPORTED_FRAME_TYPE:
            return "unsupported rpc frame type";
        case RPC_SERVICE_NOT_FOUND:
            return "rpc service not found";
        case RPC_METHOD_NOT_FOUND:
            return "rpc method not found";
        case RPC_PARSE_REQUEST_FAILED:
            return "failed to parse rpc request payload";
        case RPC_SERIALIZE_RESPONSE_FAILED:
            return "failed to serialize rpc response payload";
        case RPC_INVOKE_FAILED:
            return "rpc method invoke failed";
        case RPC_TIMEOUT:
            return "rpc request timeout";
        case RPC_CANCELLED:
            return "rpc request cancelled";
        case RPC_STREAM_NOT_FOUND:
            return "rpc stream not found";
        case RPC_STREAM_CLOSED:
            return "rpc stream closed";
        case RPC_STREAM_CANCELLED:
            return "rpc stream cancelled";
        case RPC_BACKPRESSURE:
            return "rpc backpressure triggered";
        case RPC_RESOURCE_EXHAUSTED:
            return "rpc resource exhausted";
        default:
            return "unrecognized rpc error";
        }
    }

private:
    RpcStatus(RpcErrorCode code, std::string errorText)
        : errorCode_(code), errorText_(std::move(errorText)) {
    }

private:
    RpcErrorCode errorCode_{RPC_OK};
    std::string errorText_;
};

} // namespace novanet::rpc