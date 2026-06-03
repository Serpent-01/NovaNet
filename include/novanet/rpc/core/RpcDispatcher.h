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
 * Phase 4 企业级职责：
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
 * 关键设计：
 * - RpcDispatcher 不持有 StreamManager；
 * - StreamManager 是连接级对象，由 RpcServer 每次 dispatch 时传入；
 * - immediateResponses 只用于 unary / heartbeat / immediate error；
 * - STREAM_OPEN 后续 DATA/END 通过 StreamResponder 异步发送；
 * - STREAM_OPEN 只 createStream + submit AiExecutor task；
 * - AiProvider 使用 callback/sink streaming，不返回 vector。
 */
class RpcDispatcher final {
public:
    /*
     * 注意：
     * 构造函数不再接收 StreamManager&。
     *
     * 原因：
     * stream_id 是连接内唯一，不是全服务器全局唯一。
     * 每个 TcpConnection 应有自己的 StreamManager。
     */
    RpcDispatcher(ServiceRegistry& registry, MethodInvoker& invoker,
                  AiProvider& aiProvider, AiExecutor& aiExecutor);

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;

    RpcDispatcher(RpcDispatcher&&) = delete;
    RpcDispatcher& operator=(RpcDispatcher&&) = delete;

    ~RpcDispatcher() = default;

    /*
     * 当前连接的 StreamManager 由 RpcServer 传入。
     *
     * immediateResponses:
     *   立即响应：
     *   - UNARY_RESPONSE
     *   - HEARTBEAT_PONG
     *   - ERROR_FRAME
     *
     * responder:
     *   当前连接的 StreamResponder。
     *   STREAM_OPEN 成功后，AI worker 通过 responder 发送 STREAM_DATA/STREAM_END。
     */
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
    [[nodiscard]] bool validateRegisteredMethod(const std::string& serviceName,
                                                const std::string& methodName,
                                                meta::RpcErrorCode& errorCode,
                                                std::string& errorText) const;

    [[nodiscard]] bool isSupportedStreamingMethod(const std::string& serviceName,
                                                  const std::string& methodName,
                                                  meta::RpcErrorCode& errorCode,
                                                  std::string& errorText) const;

    [[nodiscard]] bool submitGenerateTask(std::uint32_t streamId, std::uint64_t requestId,
                                          novanet::ai::chat::GenerateRequest request,
                                          std::shared_ptr<StreamResponder> responder,
                                          meta::RpcErrorCode& errorCode,
                                          std::string& errorText);

    /*
     * 在 AiExecutor worker 线程执行。
     *
     * static 的原因：
     * - 不捕获 this；
     * - 避免异步任务访问已经析构的 RpcDispatcher；
     * - 只依赖 AiProvider& 和 StreamResponder。
     */
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

    /*
     * RpcDispatcher 只依赖 AiProvider 抽象，不依赖 FakeAiProvider。
     */
    AiProvider& aiProvider_;

    /*
     * STREAM_OPEN 后提交任务到 AiExecutor，避免阻塞 EventLoop。
     */
    AiExecutor& aiExecutor_;
};

}  // namespace novanet::rpc