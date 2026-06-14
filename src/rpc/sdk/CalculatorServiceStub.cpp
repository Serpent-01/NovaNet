#include "novanet/rpc/sdk/CalculatorServiceStub.h"

#include <utility>

#include "novanet/base/Logger.h"

namespace novanet::rpc::sdk {

namespace meta = novanet::rpc::meta;

CalculatorServiceStub::CalculatorServiceStub(std::shared_ptr<ClientChannel> channel)
    : StubBase(std::move(channel)) {
}

novanet::rpc::RpcStatus CalculatorServiceStub::Add(
    ClientContext* ctx, const novanet::example::calculator::AddRequest& request,
    novanet::example::calculator::AddResponse* response) {
    if (response == nullptr) {
        const std::string error = "CalculatorServiceStub::Add response is null";
        setContextError(ctx, meta::RPC_BAD_REQUEST, error);
        return makeErrorStatus(meta::RPC_BAD_REQUEST, error);
    }

    if (ctx != nullptr && ctx->cancelled()) {
        const std::string reason = ctx->cancelReason();
        setContextError(ctx, meta::RPC_CANCELLED, reason);
        return makeErrorStatus(meta::RPC_CANCELLED, reason);
    }

    /*
     * 确保底层 ClientChannel 可用。
     *
     * 注意：
     * ClientChannel::callUnary() 内部也会 ensureConnected()。
     * 这里提前检查，是为了让 Stub 层错误更清晰。
     */
    auto readyStatus = ensureChannelReady();
    if (!readyStatus.ok()) {
        setContextError(ctx, readyStatus.errorCode(), readyStatus.errorText());

        LOG_ERROR << "[CalculatorServiceStub] channel not ready: "
                  << readyStatus.toString();

        return readyStatus;
    }

    auto status =
        channel()->callUnary(kServiceName, kAddMethodName, request, response, ctx);

    if (!status.ok()) {
        setContextError(ctx, status.errorCode(), status.errorText());

        LOG_WARN << "[CalculatorServiceStub] Add failed: " << status.toString();

        return status;
    }

    if (ctx != nullptr) {
        ctx->clearError();
    }

    return status;
}

novanet::rpc::RpcStatus CalculatorServiceStub::makeErrorStatus(
    meta::RpcErrorCode errorCode, const std::string& errorText) {
    return novanet::rpc::RpcStatus::failure(errorCode, errorText);
}

void CalculatorServiceStub::setContextError(ClientContext* ctx,
                                            meta::RpcErrorCode errorCode,
                                            const std::string& errorText) {
    if (ctx != nullptr) {
        ctx->setError(errorCode, errorText);
    }
}

}  // namespace novanet::rpc::sdk