#pragma once

#include <google/protobuf/message.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "novanet/rpc/core/RpcChannel.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/Endpoint.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

class RpcClient;

namespace sdk {

class ClientChannel final : public std::enable_shared_from_this<ClientChannel> {
public:
    using StreamCallbacks = novanet::rpc::RpcChannel::StreamCallbacks;
    using StreamHandle = novanet::rpc::RpcChannel::StreamHandle;

    [[nodiscard]] static std::shared_ptr<ClientChannel> create(
        Endpoint endpoint, ChannelOptions options = ChannelOptions{});

    ~ClientChannel();

    ClientChannel(const ClientChannel&) = delete;
    ClientChannel& operator=(const ClientChannel&) = delete;

    ClientChannel(ClientChannel&&) = delete;
    ClientChannel& operator=(ClientChannel&&) = delete;

    /*
     * 建立连接。
     *
     * 并发语义：
     * - 多线程同时调用 connect() 时，只允许一个线程真正创建 RpcClient 并连接；
     * - 其他线程等待连接结果；
     * - 连接成功后全部看到 connected；
     * - 连接失败后等待线程返回同一个失败结果，不会同时创建多个 RpcClient。
     */
    [[nodiscard]] novanet::rpc::RpcStatus connect();

    /*
     * 关闭连接。
     *
     * shutdown 后该 ClientChannel 不再复用。
     */
    void shutdown();

    [[nodiscard]] bool connected() const;

    [[nodiscard]] const Endpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] const ChannelOptions& options() const noexcept {
        return options_;
    }

    [[nodiscard]] novanet::rpc::RpcStatus callUnary(
        const std::string& serviceName, const std::string& methodName,
        const google::protobuf::Message& request,
        google::protobuf::Message* response, ClientContext* context);

    [[nodiscard]] StreamHandle openStream(const std::string& serviceName,
                                          const std::string& methodName,
                                          const google::protobuf::Message& request,
                                          ClientContext* context,
                                          StreamCallbacks callbacks);

    [[nodiscard]] bool cancelStream(std::uint32_t streamId,
                                    std::string reason = "client cancelled");

private:
    enum class State : std::uint8_t {
        kIdle = 0,
        kConnecting,
        kConnected,
        kShutdown,
    };

private:
    ClientChannel(Endpoint endpoint, ChannelOptions options);

    [[nodiscard]] novanet::rpc::RpcStatus ensureConnected();

    [[nodiscard]] static novanet::rpc::RpcStatus makeOkStatus();

    [[nodiscard]] static novanet::rpc::RpcStatus makeErrorStatus(
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText);

    void applyDefaultDeadlineForUnaryIfNeeded(ClientContext& context) const;

    void fillContextError(ClientContext* context,
                          novanet::rpc::meta::RpcErrorCode errorCode,
                          const std::string& errorText) const;

    [[nodiscard]] static const char* stateToString(State state) noexcept;

private:
    Endpoint endpoint_;
    ChannelOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable stateCv_;

    /*
     * RpcClient 内部已经持有：
     * - EventLoopThread
     * - TcpClient
     * - RpcChannel
     *
     * 所以 ClientChannel 不再自己持有 EventLoopThread。
     */
    std::shared_ptr<novanet::rpc::RpcClient> rpcClient_;

    /*
     * 由 mutex_ 保护。
     */
    State state_{State::kIdle};

    /*
     * 最近一次连接失败原因。
     * 用于并发 connect 的等待线程返回同一个失败结果。
     */
    std::string lastConnectError_;
};

}  // namespace sdk
}  // namespace novanet::rpc