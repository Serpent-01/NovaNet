#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamMethodInvoker.h"
#include "novanet/rpc/core/StreamResponder.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

class RpcDispatcher final {
public:
    RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker,
                  StreamMethodInvoker& streamInvoker);

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;
    RpcDispatcher(RpcDispatcher&&) = delete;
    RpcDispatcher& operator=(RpcDispatcher&&) = delete;
    ~RpcDispatcher() = default;

    [[nodiscard]] bool dispatch(const RpcMessage& msg, StreamManager& streamManager,
                                std::vector<RpcMessage>& immediateResponses,
                                const std::shared_ptr<StreamResponder>& responder);

private:
    [[nodiscard]] bool dispatchUnaryRequest(const RpcMessage& msg,
                                            std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamOpen(
        const RpcMessage& msg, StreamManager& streamManager,
        std::vector<RpcMessage>& immediateResponses,
        const std::shared_ptr<StreamResponder>& responder);

    [[nodiscard]] bool dispatchStreamData(const RpcMessage& msg,
                                          StreamManager& streamManager,
                                          std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamEnd(const RpcMessage& msg,
                                         StreamManager& streamManager,
                                         std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamCancel(const RpcMessage& msg,
                                            StreamManager& streamManager,
                                            std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchHeartbeatPing(const RpcMessage& msg,
                                             std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchHeartbeatPong(const RpcMessage& msg,
                                             std::vector<RpcMessage>& immediateResponses);

private:
    [[nodiscard]] bool appendUnaryOkResponse(
        const RpcMessage& requestMsg, std::string responsePayload,
        std::vector<RpcMessage>& immediateResponses) const;

    [[nodiscard]] bool appendUnaryErrorResponse(
        const RpcMessage& requestMsg, meta::RpcErrorCode errorCode, std::string errorText,
        std::vector<RpcMessage>& immediateResponses) const;

    [[nodiscard]] bool appendHeartbeatPong(
        const RpcMessage& requestMsg, std::vector<RpcMessage>& immediateResponses) const;

    [[nodiscard]] bool appendErrorFrame(
        const RpcMessage& requestMsg, meta::RpcErrorCode errorCode, std::string errorText,
        std::vector<RpcMessage>& immediateResponses) const;

private:
    [[nodiscard]] static meta::RpcErrorCode streamResultToErrorCode(
        StreamManager::Result result) noexcept;

private:
    ServiceRegistry& registry_;
    MethodInvoker& invoker_;
    StreamMethodInvoker& streamInvoker_;
};

}  // namespace novanet::rpc
