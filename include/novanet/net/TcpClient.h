#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpConnection.h"

namespace novanet::net {

class Channel;
class EventLoop;

/*
 * TcpClient 是 NovaNet 客户端 TCP 连接管理对象。
 *
 * 职责：
 * - 在指定 EventLoop 上发起非阻塞 connect；
 * - 管理 connecting 阶段的 connector fd / connector Channel；
 * - 连接成功后创建 TcpConnection；
 * - 提供 connect / disconnect / stop；
 * - 通过 weak_ptr 捕获异步回调，避免对象析构后 EventLoop 执行旧任务造成 UAF。
 *
 * 生命周期要求：
 * - TcpClient 必须由 std::shared_ptr 管理；
 * - 推荐 std::make_shared<TcpClient>(loop, addr, name)；
 * - 不要栈上创建；
 * - 不要用 std::unique_ptr 创建；
 * - 上层 RpcClient 应在 reset TcpClient 前调用 stop() 并等待 closeCompleteCallback。
 */
class TcpClient final : public std::enable_shared_from_this<TcpClient> {
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

    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

    /*
     * 发起异步连接。
     *
     * 线程安全语义：
     * - 可以从任意线程调用；
     * - 内部通过 loop_->runInLoop 投递到 loop 线程；
     * - 投递任务捕获 weak_ptr<TcpClient>，避免 UAF。
     */
    void connect();

    /*
     * 优雅断开。
     *
     * 如果正在 connecting：
     * - 取消 connectorChannel；
     * - 关闭 connectorFd；
     * - 触发 closeCompleteCallback。
     *
     * 如果已经 connected：
     * - 调用 TcpConnection::shutdown()。
     */
    void disconnect();

    /*
     * 快速停止。
     *
     * 如果已经 connected：
     * - 调用 TcpConnection::forceClose()。
     */
    void stop();

    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] TcpConnectionPtr connection() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setConnectErrorCallback(ConnectErrorCallback cb);
    void setCloseCompleteCallback(CloseCompleteCallback cb);

private:
    enum class State : std::uint8_t {
        kDisconnected = 0,
        kConnecting,
        kConnected,
        kDisconnecting,
    };

private:
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

    /*
     * 获取 weak_ptr。
     *
     * 如果对象不是由 shared_ptr 管理，weak_from_this() 会是空。
     * 这种情况下不能继续投递异步任务，否则生命周期无法保证。
     */
    [[nodiscard]] std::weak_ptr<TcpClient> weakSelf() noexcept;

    [[nodiscard]] static const char* stateToString(State state) noexcept;

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