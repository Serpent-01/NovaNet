#include "novanet/rpc/sdk/ClientChannel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
}

}  // namespace novanet::rpc::sdk