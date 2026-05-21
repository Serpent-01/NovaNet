#pragma once

#include <string>
#include <vector>

#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "rpc_meta.pb.h"
namespace novanet::rpc {

class RpcDispatcher final {
public:
    RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker);

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;

    RpcDispatcher(RpcDispatcher&&) = delete;
    RpcDispatcher& operator=(RpcDispatcher&&) = delete;

    ~RpcDispatcher() = default;

    [[nodiscard]] bool dispatch(const RpcMessage& msg,
                                std::vector<RpcMessage>& outResponses) const;

private:
    [[nodiscard]] bool dispatchUnaryRequest(
        const RpcMessage& msg, std::vector<RpcMessage>& outResponses) const;

    [[nodiscard]] bool appendUnaryOkResponse(
        const RpcMessage& requestMsg, std::string responsePayload,
        std::vector<RpcMessage>& outResponses) const;

    [[nodiscard]] bool appendUnaryErrorResponse(
        const RpcMessage& requestMsg, RpcErrorCode errorCode,
        std::string errorText, std::vector<RpcMessage>& outResponses) const;

    [[nodiscard]] bool appendErrorFrame(
        const RpcMessage& requestMsg, RpcErrorCode errorCode,
        std::string errorText, std::vector<RpcMessage>& outResponses) const;

private:
    ServiceRegistry& registry_;
    MethodInvoker& invoker_;
};

}  // namespace novanet::rpc