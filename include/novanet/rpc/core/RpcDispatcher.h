#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chat.pb.h"
#include "novanet/rpc/core/AiExecutor.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamResponder.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * RpcDispatcher 是服务端 RPC 语义分发入口。
 *
 * - 只做 RpcMessage 语义分发；
 * - 不读写 socket；
 * - 不处理 Buffer；
 * - 不处理半包 / 粘包；
 * - 不直接依赖 FakeAiProvider；
 * - 不在 EventLoop 线程里执行 AI 生成；
 * - 不直接调用真实 AI API；
 * - 不 curl / fork / HTTP SSE；
 * - 不直接操作 TcpConnection。
 *
 * 关键改动：
 * - dispatch() 不再只返回 outResponses；
 * - unary / heartbeat / 立即错误仍然放入 immediateResponses；
 * - STREAM_OPEN 后续 DATA/END 通过 StreamResponder 异步发送。
 */
class RpcDispatcher final {
public:
    RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker,
                  StreamManager& streamManager, AiProvider& aiProvider,
                  AiExecutor& aiExecutor);

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;

    RpcDispatcher(RpcDispatcher&&) = delete;
    RpcDispatcher& operator=(RpcDispatcher&&) = delete;

    ~RpcDispatcher() = default;

    [[nodiscard]] bool dispatch(const RpcMessage& msg,
                                std::vector<RpcMessage>& immediateResponses,
                                const std::shared_ptr<StreamResponder>& responder);

private:
    [[nodiscard]] bool dispatchUnaryRequest(const RpcMessage& msg,
                                            std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamOpen(
        const RpcMessage& msg, std::vector<RpcMessage>& immediateResponses,
        const std::shared_ptr<StreamResponder>& responder);

    [[nodiscard]] bool dispatchStreamData(const RpcMessage& msg,
                                          std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamEnd(const RpcMessage& msg,
                                         std::vector<RpcMessage>& immediateResponses);

    [[nodiscard]] bool dispatchStreamCancel(const RpcMessage& msg,
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
    [[nodiscard]] bool validateRegisteredMethod(const std::string& serviceName,
                                                const std::string& methodName,
                                                meta::RpcErrorCode& errorCode,
                                                std::string& errorText) const;

    [[nodiscard]] bool submitGenerateTask(std::uint32_t streamId, std::uint64_t requestId,
                                          novanet::ai::chat::GenerateRequest request,
                                          std::shared_ptr<StreamResponder> responder,
                                          meta::RpcErrorCode& errorCode,
                                          std::string& errorText);

    static void runGenerateTask(AiProvider& aiProvider,
                                std::shared_ptr<StreamResponder> responder,
                                std::uint32_t streamId, std::uint64_t requestId,
                                novanet::ai::chat::GenerateRequest request);

    [[nodiscard]] static meta::RpcErrorCode aiStatusToErrorCode(
        AiProvider::StatusCode code) noexcept;

    [[nodiscard]] static meta::RpcErrorCode executorSubmitResultToErrorCode(
        AiExecutor::SubmitResult result) noexcept;

    [[nodiscard]] static meta::RpcErrorCode streamResultToErrorCode(
        StreamManager::Result result) noexcept;

    [[nodiscard]] static std::string defaultErrorText(const std::string& text,
                                                      const char* fallback);

private:
    ServiceRegistry& registry_;
    MethodInvoker& invoker_;

    StreamManager& streamManager_;

    AiProvider& aiProvider_;

    AiExecutor& aiExecutor_;
};

}  // namespace novanet::rpc