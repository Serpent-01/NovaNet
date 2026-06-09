#include "novanet/rpc/sdk/ClientChannel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcClient.h"
#include "novanet/rpc/core/RpcStatus.h"

namespace novanet::rpc::sdk {

namespace meta = novanet::rpc::meta;

namespace {

std::chrono::milliseconds toMilliseconds(double seconds) {
    if (seconds <= 0.0) {
        return std::chrono::milliseconds(0);
    }

    const auto millis = static_cast<std::int64_t>(seconds * 1000.0);

    return std::chrono::milliseconds(std::max<std::int64_t>(1, millis));
}

novanet::rpc::RpcClient::Options makeRpcClientOptions(
    const ChannelOptions& options) {
    novanet::rpc::RpcClient::Options clientOptions;

    clientOptions.connectTimeout = toMilliseconds(options.connectTimeoutSeconds);

    clientOptions.startHeartbeatTimers = options.startHeartbeat;

    clientOptions.channelOptions.sendHighWaterMarkBytes =
        options.sendHighWaterMarkBytes;

    clientOptions.channelOptions.heartbeatIntervalSeconds =
        options.heartbeatIntervalSeconds;

    clientOptions.channelOptions.heartbeatCheckIntervalSeconds =
        options.heartbeatCheckIntervalSeconds;

    clientOptions.channelOptions.heartbeatTimeoutSeconds =
        options.heartbeatTimeoutSeconds;

    clientOptions.channelOptions.streamTimeoutScanIntervalSeconds =
        options.streamTimeoutScanIntervalSeconds;

    clientOptions.channelOptions.streamIdleTimeoutSeconds =
        options.streamIdleTimeoutSeconds;

    clientOptions.channelOptions.nodeId = options.nodeId;

    return clientOptions;
}

}  // namespace

std::shared_ptr<ClientChannel> ClientChannel::create(Endpoint endpoint,
                                                     ChannelOptions options) {
    if (!endpoint.valid()) {
        LOG_ERROR << "[ClientChannel] create failed: invalid endpoint";
        return nullptr;
    }

    return std::shared_ptr<ClientChannel>(
        new ClientChannel(std::move(endpoint), std::move(options)));
}

ClientChannel::ClientChannel(Endpoint endpoint, ChannelOptions options)
    : endpoint_(std::move(endpoint)), options_(std::move(options)) {
}

ClientChannel::~ClientChannel() {
    shutdown();
}

novanet::rpc::RpcStatus ClientChannel::connect() {
    std::shared_ptr<novanet::rpc::RpcClient> client;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::kShutdown) {
            return makeErrorStatus(meta::RPC_CONNECTION_CLOSED,
                                   "ClientChannel already shutdown");
        }

        if (state_ == State::kConnected) {
            if (rpcClient_ != nullptr && rpcClient_->connected()) {
                return makeOkStatus();
            }
            LOG_WARN << "[ClientChannel] state connected but RpcClient is not "
                        "connected, target="
                     << endpoint_.toString();

            rpcClient_.reset();
            state_ = State::kIdle;
        }

        if (state_ == State::kConnecting) {
            //如果已经有线程在连接，当前线程不再创建新的
            // RpcClient，而是等待连接结果。
            stateCv_.wait(lock, [this]() { return state_ != State::kConnecting; });

            if (state_ == State::kConnected && rpcClient_ != nullptr &&
                rpcClient_->connected()) {
                return makeOkStatus();
            }
            if (state_ == State::kShutdown) {
                return makeErrorStatus(meta::RPC_CONNECTION_CLOSED,
                                       "ClientChannel shutdown during connect");
            }
            /*
             * 连接线程已经失败。
             * 作为并发等待者，直接返回同一个失败结果，不再立刻发起第二次连接。
             * 后续用户再次显式调用 connect()，才会重新尝试。
             */
            const std::string error =
                lastConnectError_.empty() ? "connect failed" : lastConnectError_;

            return makeErrorStatus(meta::RPC_CONNECTION_CLOSED, error);
        }
        /*
         * State::kIdle:
         * 当前线程成为唯一连接线程。
         */
        novanet::net::InetAddress serverAddr(endpoint_.host(), endpoint_.port());

        auto clientOptions = makeRpcClientOptions(options_);

        rpcClient_ = std::make_shared<novanet::rpc::RpcClient>(
            serverAddr, endpoint_.toString(), std::move(clientOptions));

        client = rpcClient_;

        state_ = State::kConnecting;
        lastConnectError_.clear();
    }

    /*
     * 不持有 ClientChannel::mutex_ 执行真正连接。
     * RpcClient::connect() 内部会等待 TcpClient 连接完成。
     */
    std::string errorText;

    const bool ok = client->connect(&errorText);

    if (!ok) {
        if (errorText.empty()) {
            errorText = "connect failed";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::kShutdown) {
                /*
                 * shutdown 期间 connect 失败。
                 * 状态保持 shutdown。
                 */
                if (rpcClient_ == client) {
                    rpcClient_.reset();
                }
                lastConnectError_ = "ClientChannel shutdown during connect";
            } else if (rpcClient_ == client) {
                rpcClient_.reset();
                state_ = State::kIdle;
                lastConnectError_ = errorText;
            } else {
                /*
                 * 理论上不会发生。
                 * 但防御：不要误改别的 client。
                 */
                lastConnectError_ = errorText;
            }
        }
        stateCv_.notify_all();
        LOG_ERROR << "[ClientChannel] connect failed, target="
                  << endpoint_.toString() << ", error=" << errorText;

        return makeErrorStatus(meta::RPC_CONNECTION_CLOSED, std::move(errorText));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kShutdown) {
            /*
             * 极端情况：
             * connect 成功返回前，另一个线程已经 shutdown。
             * 不能把状态改回 connected。
             */
            if (rpcClient_ == client) {
                rpcClient_.reset();
            }
            lastConnectError_ = "ClientChannel shutdown during connect";
            stateCv_.notify_all();
            /*
             * 不在锁内 disconnect。
             */
        } else if (rpcClient_ == client) {
            state_ = State::kConnected;
            lastConnectError_.clear();

            stateCv_.notify_all();

            LOG_INFO << "[ClientChannel] connected, target=" << endpoint_.toString();

            return makeOkStatus();
        } else {
            lastConnectError_ = "active RpcClient changed during connect";
            stateCv_.notify_all();
        }
    }
    /*
     * 走到这里说明 connect 期间被 shutdown 或 active client 变化。
     */
    client->disconnect();

    return makeErrorStatus(meta::RPC_CONNECTION_CLOSED,
                           "ClientChannel shutdown during connect");
}

void ClientChannel::shutdown() {
    std::shared_ptr<novanet::rpc::RpcClient> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == State::kShutdown) {
            return;
        }

        state_ = State::kShutdown;
        lastConnectError_ = "ClientChannel shutdown";

        client = std::move(rpcClient_);
        rpcClient_.reset();
    }
    stateCv_.notify_all();

    /*
     * 不在锁内调用 RpcClient::disconnect()，避免阻塞其他线程。
     */
    if (client) {
        client->disconnect();
    }
    LOG_INFO << "[ClientChannel] shutdown, target=" << endpoint_.toString();
}

bool ClientChannel::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return state_ == State::kConnected && rpcClient_ != nullptr &&
           rpcClient_->connected();
}

novanet::rpc::RpcStatus ClientChannel::callUnary(
    const std::string& serviceName, const std::string& methodName,
    const google::protobuf::Message& request, google::protobuf::Message* response,
    ClientContext* context) {
    ClientContext localContext;
    ClientContext& ctx = context == nullptr ? localContext : *context;

    if (serviceName.empty()) {
        const std::string error = "serviceName is empty";
        ctx.setError(meta::RPC_BAD_REQUEST, error);
        return makeErrorStatus(meta::RPC_BAD_REQUEST, error);
    }
    if (methodName.empty()) {
        const std::string error = "methodName is empty";
        ctx.setError(meta::RPC_BAD_REQUEST, error);
        return makeErrorStatus(meta::RPC_BAD_REQUEST, error);
    }
    if (response == nullptr) {
        const std::string error = "response is null";
        ctx.setError(meta::RPC_BAD_REQUEST, error);
        return makeErrorStatus(meta::RPC_BAD_REQUEST, error);
    }

    if (ctx.cancelled()) {
        const std::string reason = ctx.cancelReason();
        ctx.setError(meta::RPC_CANCELLED, reason);
        return makeErrorStatus(meta::RPC_CANCELLED, reason);
    }

    /*
     * unary 默认使用 ChannelOptions.defaultRpcTimeoutSeconds。
     */
    applyDefaultDeadlineForUnaryIfNeeded(ctx);

    if (ctx.expired()) {
        const std::string error = "RPC deadline expired before send";
        ctx.setError(meta::RPC_TIMEOUT, error);
        return makeErrorStatus(meta::RPC_TIMEOUT, error);
    }

    auto connectStatus = ensureConnected();
    if (!connectStatus.ok()) {
        fillContextError(&ctx, connectStatus.errorCode(), connectStatus.errorText());
        return connectStatus;
    }

    std::shared_ptr<novanet::rpc::RpcClient> client;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != State::kConnected) {
            const std::string error = "ClientChannel is not connected";
            ctx.setError(meta::RPC_CONNECTION_CLOSED, error);
            return makeErrorStatus(meta::RPC_CONNECTION_CLOSED, error);
        }

        client = rpcClient_;
    }
    if (!client || !client->connected()) {
        const std::string error = "RpcClient is not connected";
        ctx.setError(meta::RPC_CONNECTION_CLOSED, error);
        return makeErrorStatus(meta::RPC_CONNECTION_CLOSED, error);
    }

    const double remainingSeconds = ctx.remainingTimeoutSeconds();
    const auto timeout = toMilliseconds(remainingSeconds);
    const auto metadata = ctx.metadata();
    std::uint64_t requestId = 0;

    novanet::rpc::RpcStatus status;
    if (context != nullptr) {
        auto cancelChecker = [&ctx]() { return ctx.cancelled(); };
        auto cancelReasonProvider = [&ctx]() { return ctx.cancelReason(); };

        status = client->callUnary(
            serviceName, methodName, request, response, timeout, metadata, &requestId,
            std::move(cancelChecker), std::move(cancelReasonProvider));
    } else {
        status = client->callUnary(serviceName, methodName, request, response, timeout,
                                   metadata, &requestId);
    }

    if (requestId != 0) {
        ctx.setRequestId(requestId);
    }

    if (!status.ok()) {
        fillContextError(&ctx, status.errorCode(), status.errorText());
        return status;
    }

    ctx.clearError();
    return status;
}

ClientChannel::StreamHandle ClientChannel::openStream(
    const std::string& serviceName, const std::string& methodName,
    const google::protobuf::Message& request, ClientContext* context,
    StreamCallbacks callbacks) {
    ClientContext localContext;
    ClientContext& ctx = context == nullptr ? localContext : *context;

    if (serviceName.empty()) {
        const std::string error = "serviceName is empty";
        ctx.setError(meta::RPC_BAD_REQUEST, error);
        return StreamHandle{0, 0, false, error};
    }

    if (methodName.empty()) {
        const std::string error = "methodName is empty";
        ctx.setError(meta::RPC_BAD_REQUEST, error);
        return StreamHandle{0, 0, false, error};
    }

    if (ctx.cancelled()) {
        const std::string reason = ctx.cancelReason();
        ctx.setError(meta::RPC_CANCELLED, reason);
        return StreamHandle{0, 0, false, reason};
    }

    /*
     * streaming 不强制套用 defaultRpcTimeoutSeconds。
     *
     * 原因：
     * - unary 是 request/response；
     * - streaming 生命周期更长；
     * - stream 空闲超时由 RpcChannel::streamIdleTimeoutSeconds 控制；
     * - 如果用户想限制整个 stream，可以手动 ctx.setTimeoutSeconds(...)。
     */
    if (ctx.expired()) {
        const std::string error = "stream deadline expired before open";
        ctx.setError(meta::RPC_TIMEOUT, error);
        return StreamHandle{0, 0, false, error};
    }

    auto connectStatus = ensureConnected();
    if (!connectStatus.ok()) {
        fillContextError(&ctx, connectStatus.errorCode(), connectStatus.errorText());
        return StreamHandle{0, 0, false, connectStatus.errorText()};
    }

    std::shared_ptr<novanet::rpc::RpcClient> client;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != State::kConnected) {
            const std::string error = "ClientChannel is not connected";
            ctx.setError(meta::RPC_CONNECTION_CLOSED, error);
            return StreamHandle{0, 0, false, error};
        }

        client = rpcClient_;
    }

    if (!client || !client->connected()) {
        const std::string error = "RpcClient is not connected";
        ctx.setError(meta::RPC_CONNECTION_CLOSED, error);
        return StreamHandle{0, 0, false, error};
    }

    const auto metadata = ctx.metadata();

    StreamHandle handle = client->openStream(serviceName, methodName, request,
                                             std::move(callbacks), metadata);

    if (!handle.ok) {
        ctx.setError(meta::RPC_UNKNOWN_ERROR, handle.errorText);
        return handle;
    }

    ctx.setRequestId(handle.requestId);
    ctx.setStreamId(handle.streamId);
    ctx.clearError();

    LOG_INFO << "[ClientChannel] stream opened, target=" << endpoint_.toString()
             << ", streamId=" << handle.streamId
             << ", requestId=" << handle.requestId << ", service=" << serviceName
             << ", method=" << methodName;

    return handle;
}

bool ClientChannel::cancelStream(std::uint32_t streamId, std::string reason) {
    std::shared_ptr<novanet::rpc::RpcClient> client;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != State::kConnected) {
            return false;
        }

        client = rpcClient_;
    }

    if (!client || !client->connected()) {
        return false;
    }

    return client->cancelStream(streamId, std::move(reason));
}

novanet::rpc::RpcStatus ClientChannel::ensureConnected() {
    if (connected()) {
        return makeOkStatus();
    }

    return connect();
}

novanet::rpc::RpcStatus ClientChannel::makeOkStatus() {
    return novanet::rpc::RpcStatus::success();
}

novanet::rpc::RpcStatus ClientChannel::makeErrorStatus(meta::RpcErrorCode errorCode,
                                                       std::string errorText) {
    return novanet::rpc::RpcStatus::failure(errorCode, std::move(errorText));
}

void ClientChannel::applyDefaultDeadlineForUnaryIfNeeded(
    ClientContext& context) const {
    if (!context.hasDeadline()) {
        context.setTimeoutSeconds(options_.defaultRpcTimeoutSeconds);
    }
}

void ClientChannel::fillContextError(ClientContext* context,
                                     meta::RpcErrorCode errorCode,
                                     const std::string& errorText) const {
    if (context != nullptr) {
        context->setError(errorCode, errorText);
    }
}

const char* ClientChannel::stateToString(State state) noexcept {
    switch (state) {
        case State::kIdle:
            return "Idle";
        case State::kConnecting:
            return "Connecting";
        case State::kConnected:
            return "Connected";
        case State::kShutdown:
            return "Shutdown";
        default:
            return "Unknown";
    }
}

}  // namespace novanet::rpc::sdk
