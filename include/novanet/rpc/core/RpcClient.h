#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "novanet/net/EventLoopThread.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpClient.h"
#include "novanet/rpc/core/RpcChannel.h"
#include "novanet/rpc/core/RpcStatus.h"

namespace google::protobuf {
class Message;
}

namespace novanet::rpc {

class RpcClient final {
public:
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

    [[nodiscard]] bool connect(std::string* errorText = nullptr);
    void disconnect();

    [[nodiscard]] bool connected() const;

    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout);

    [[nodiscard]] RpcChannel::StreamHandle openStream(
        const std::string& serviceName, const std::string& methodName,
        const google::protobuf::Message& request,
        RpcChannel::StreamCallbacks callbacks);

    [[nodiscard]] bool cancelStream(std::uint32_t streamId,
                                    std::string reason = "client cancelled");

    [[nodiscard]] bool sendHeartbeatPing();

private:
    [[nodiscard]] std::shared_ptr<RpcChannel> channelSnapshot() const;
    void notifyConnected();
    void notifyClosed(std::string reason);
    void notifyCloseComplete();
    void notifyConnectError(std::string reason);

private:
    novanet::net::InetAddress serverAddr_;
    std::string name_;
    Options options_;

    novanet::net::EventLoopThread loopThread_;
    novanet::net::EventLoop* loop_{nullptr};
    std::unique_ptr<novanet::net::TcpClient> tcpClient_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool started_{false};
    bool connected_{false};
    bool connectFinished_{false};
    bool closeComplete_{true};
    std::string lastError_;
    std::shared_ptr<RpcChannel> channel_;
};

}  // namespace novanet::rpc
