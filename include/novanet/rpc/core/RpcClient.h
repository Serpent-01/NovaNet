#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "novanet/net/EventLoopThread.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpClient.h"
#include "novanet/rpc/core/RpcChannel.h"
#include "novanet/rpc/core/RpcStatus.h"

namespace google::protobuf {
class Message;
}

namespace novanet::rpc {

/*
 * RpcClient 是客户端底层连接对象。
 *
 * 职责：
 * - 持有客户端 EventLoopThread；
 * - 持有 TcpClient；
 * - 连接建立后创建 RpcChannel；
 * - 对上层 ClientChannel 提供 callUnary / openStream / cancelStream；
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试；
 * - 不做认证 / 拦截器。
 *
 * 生命周期要求：
 * - RpcClient 必须由 std::shared_ptr 管理；
 * - 普通异步回调使用 weak_from_this() 防止 UAF；
 * - 关闭完成回调由 disconnect() 等待，并在 EventLoop 线程 join 前完成；
 * - ClientChannel 里应该使用 std::make_shared<RpcClient>(...) 创建。
 */
class RpcClient final : public std::enable_shared_from_this<RpcClient> {
public:
    using MetadataMap = std::unordered_map<std::string, std::string>;

    struct Options {
        RpcChannel::Options channelOptions{};
        std::chrono::milliseconds connectTimeout{3000};
        bool startHeartbeatTimers{false};
    };

    RpcClient(const novanet::net::InetAddress& serverAddr, std::string name);

    RpcClient(const novanet::net::InetAddress& serverAddr, std::string name,
              Options options);

    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    RpcClient(RpcClient&&) = delete;
    RpcClient& operator=(RpcClient&&) = delete;

    [[nodiscard]] bool connect(std::string* errorText = nullptr);

    void disconnect();

    [[nodiscard]] bool connected() const;

    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout);

    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout,
                                      const MetadataMap& metadata);

    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout,
                                      const MetadataMap& metadata,
                                      std::uint64_t* requestIdOut);

    [[nodiscard]] RpcChannel::StreamHandle openStream(
        const std::string& serviceName, const std::string& methodName,
        const google::protobuf::Message& request,
        RpcChannel::StreamCallbacks callbacks);

    [[nodiscard]] RpcChannel::StreamHandle openStream(
        const std::string& serviceName, const std::string& methodName,
        const google::protobuf::Message& request,
        RpcChannel::StreamCallbacks callbacks, const MetadataMap& metadata);

    [[nodiscard]] bool cancelStream(std::uint32_t streamId,
                                    std::string reason = "client cancelled");

    [[nodiscard]] bool sendHeartbeatPing();

private:
    enum class State : std::uint8_t {
        kIdle = 0,
        kConnecting,
        kConnected,
        kClosing,
        kClosed,
    };

private:
    [[nodiscard]] std::shared_ptr<RpcChannel> channelSnapshot() const;
    [[nodiscard]] std::weak_ptr<RpcClient> weakSelfOrDie();

    void handleTcpConnection(
        const novanet::net::TcpConnection::TcpConnectionPtr& conn);

    void handleTcpMessage(const novanet::net::TcpConnection::TcpConnectionPtr& conn,
                          novanet::net::Buffer* buffer);

    void handleTcpConnectError(int errorCode, std::string error);
    void handleTcpCloseComplete();

    void notifyConnected();
    void notifyClosed(std::string reason);
    void notifyCloseComplete();
    void notifyConnectError(std::string reason);

    [[nodiscard]] static const char* stateToString(State state) noexcept;

private:
    novanet::net::InetAddress serverAddr_;
    std::string name_;
    Options options_;

    novanet::net::EventLoopThread loopThread_;
    novanet::net::EventLoop* loop_{nullptr};
    std::shared_ptr<novanet::net::TcpClient> tcpClient_;

    std::mutex disconnectMutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    State state_{State::kIdle};
    bool closeComplete_{true};
    bool disconnectComplete_{true};

    std::string lastError_;
    std::shared_ptr<RpcChannel> channel_;
};

}  // namespace novanet::rpc
