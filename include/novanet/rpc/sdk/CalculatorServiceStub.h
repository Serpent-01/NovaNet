#pragma once

#include <memory>

#include "calculator.pb.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ClientChannel.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/StubBase.h"

namespace novanet::rpc::sdk {

/*
 * CalculatorServiceStub 是 CalculatorService 的强类型 SDK Stub。
 *
 * Phase 4 SDK 目标：
 *
 *   auto channel = novanet::rpc::CreateChannel("127.0.0.1:19090");
 *
 *   CalculatorServiceStub calculator(channel);
 *
 *   ClientContext ctx;
 *   AddRequest req;
 *   AddResponse resp;
 *
 *   auto status = calculator.Add(&ctx, req, &resp);
 *
 * 职责：
 * - 提供 Add() 强类型 unary API；
 * - 内部调用 ClientChannel::callUnary()；
 * - 不直接接触 RpcClient / RpcChannel / TcpConnection；
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试；
 * - 不做认证 / 拦截器。
 */
class CalculatorServiceStub final : public StubBase {
public:
    explicit CalculatorServiceStub(std::shared_ptr<ClientChannel> channel);

    ~CalculatorServiceStub() override = default;

    CalculatorServiceStub(const CalculatorServiceStub&) = default;
    CalculatorServiceStub& operator=(const CalculatorServiceStub&) = default;

    CalculatorServiceStub(CalculatorServiceStub&&) noexcept = default;
    CalculatorServiceStub& operator=(CalculatorServiceStub&&) noexcept = default;

    /*
     * CalculatorService.Add unary 调用。
     *
     * ctx:
     * - 可以为 nullptr；
     * - 如果不为 nullptr，会携带 timeout / metadata / cancel；
     * - 错误时会写入 ctx->setError(...)。
     *
     * request:
     * - AddRequest 强类型请求。
     *
     * response:
     * - AddResponse 强类型响应；
     * - 不允许为 nullptr。
     */
    [[nodiscard]] novanet::rpc::RpcStatus Add(
        ClientContext* ctx, const novanet::example::calculator::AddRequest& request,
        novanet::example::calculator::AddResponse* response);

    [[nodiscard]] static constexpr const char* serviceName() noexcept {
        return kServiceName;
    }

    [[nodiscard]] static constexpr const char* addMethodName() noexcept {
        return kAddMethodName;
    }

private:
    [[nodiscard]] static novanet::rpc::RpcStatus makeErrorStatus(
        novanet::rpc::meta::RpcErrorCode errorCode, const std::string& errorText);

    static void setContextError(ClientContext* ctx,
                                novanet::rpc::meta::RpcErrorCode errorCode,
                                const std::string& errorText);

private:
    static constexpr const char* kServiceName =
        "novanet.example.calculator.CalculatorService";

    static constexpr const char* kAddMethodName = "Add";
};

}  // namespace novanet::rpc::sdk