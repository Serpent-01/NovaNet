#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpConnection.h"

namespace novanet::net {

class Channel;
class EventLoop;

class TcpClient final {
public:
    using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback = TcpConnection::MessageCallback;
    using WriteCompleteCallback = TcpConnection::WriteCompleteCallback;
    using ConnectErrorCallback = std::function<void(int, std::string)>;
    using CloseCompleteCallback = std::function<void()>;

    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void connect();
    void disconnect();
    void stop();

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] TcpConnectionPtr connection() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setConnectErrorCallback(ConnectErrorCallback cb);
    void setCloseCompleteCallback(CloseCompleteCallback cb);

private:
    enum class State { kDisconnected, kConnecting, kConnected };

    void connectInLoop();
    void disconnectInLoop();
    void stopInLoop();

    void handleConnectWrite();
    void handleConnectError();
    void establishConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);

    void resetConnectorChannel();
    void closeConnectorFd();
    void reportConnectError(int errorCode, std::string message);

private:
    EventLoop* loop_{nullptr};
    InetAddress serverAddr_;
    std::string name_;

    std::atomic<State> state_{State::kDisconnected};

    int connectorFd_{-1};
    std::shared_ptr<Channel> connectorChannel_;

    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ConnectErrorCallback connectErrorCallback_;
    CloseCompleteCallback closeCompleteCallback_;
};

}  // namespace novanet::net
