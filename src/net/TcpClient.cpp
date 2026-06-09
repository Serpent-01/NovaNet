#include "novanet/net/TcpClient.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/Channel.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/SocketsOps.h"

namespace novanet::net {

namespace {

void defaultConnectionCallback(const TcpConnection::TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }

    LOG_INFO << "TcpClient connection " << conn->name() << " is "
             << (conn->connected() ? "UP" : "DOWN");
}

void defaultMessageCallback(const TcpConnection::TcpConnectionPtr&, Buffer* buffer) {
    if (buffer != nullptr) {
        buffer->retrieveAll();
    }
}

}  // namespace

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr,
                     std::string name)
    : loop_(loop),
      serverAddr_(serverAddr),
      name_(std::move(name)),
      connectionCallback_(defaultConnectionCallback),
      messageCallback_(defaultMessageCallback) {
    assert(loop_ != nullptr);
}

TcpClient::~TcpClient() {
    /*
     * 析构函数不再投递 stopInLoop。
     *
     * 原因：
     * - 析构阶段 shared_ptr 生命周期已经结束；
     * - 再投递异步任务即使捕获 weak_ptr，也无法保证优雅关闭；
     * - 正确做法是上层 RpcClient::disconnect() 先调用 stop()，
     *   等 closeCompleteCallback 后再 reset TcpClient。
     */
    if (state_.load(std::memory_order_acquire) != State::kDisconnected) {
        LOG_WARN << "[TcpClient] destroyed while not disconnected, name=" << name_
                 << ", state="
                 << stateToString(state_.load(std::memory_order_acquire));
    }
}

std::weak_ptr<TcpClient> TcpClient::weakSelf() noexcept {
    return weak_from_this();
}

void TcpClient::connect() {
    auto weak = weakSelf();

    if (weak.expired()) {
        LOG_ERROR << "[TcpClient] connect requires shared ownership, name=" << name_;
        return;
    }

    loop_->runInLoop([weak]() {
        auto self = weak.lock();
        if (!self) {
            return;
        }

        self->connectInLoop();
    });
}

void TcpClient::disconnect() {
    auto weak = weakSelf();

    if (weak.expired()) {
        LOG_ERROR << "[TcpClient] disconnect requires shared ownership, name="
                  << name_;
        return;
    }

    loop_->runInLoop([weak]() {
        auto self = weak.lock();
        if (!self) {
            return;
        }

        self->disconnectInLoop();
    });
}

void TcpClient::stop() {
    auto weak = weakSelf();

    if (weak.expired()) {
        LOG_ERROR << "[TcpClient] stop requires shared ownership, name=" << name_;
        return;
    }

    loop_->runInLoop([weak]() {
        auto self = weak.lock();
        if (!self) {
            return;
        }

        self->stopInLoop();
    });
}

bool TcpClient::connected() const noexcept {
    return state_.load(std::memory_order_acquire) == State::kConnected;
}

TcpClient::TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

void TcpClient::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpClient::setConnectErrorCallback(ConnectErrorCallback cb) {
    connectErrorCallback_ = std::move(cb);
}

void TcpClient::setCloseCompleteCallback(CloseCompleteCallback cb) {
    closeCompleteCallback_ = std::move(cb);
}

void TcpClient::connectInLoop() {
    loop_->assertInLoopThread();

    const State current = state_.load(std::memory_order_acquire);
    if (current != State::kDisconnected) {
        LOG_WARN << "[TcpClient] connect ignored, name=" << name_
                 << ", state=" << stateToString(current);
        return;
    }

    connectorFd_ = sockets::createNonblockingOrDie(serverAddr_.family());
    sockets::setTcpNoDelay(connectorFd_, true);

    const int ret = sockets::connect(connectorFd_, serverAddr_.getSockAddr());
    const int savedErrno = ret == 0 ? 0 : errno;

    if (ret == 0) {
        establishConnection(connectorFd_);
        connectorFd_ = -1;
        return;
    }

    switch (savedErrno) {
        case EINPROGRESS:
        case EINTR:
        case EISCONN: {
            state_.store(State::kConnecting, std::memory_order_release);

            connectorChannel_ = std::make_shared<Channel>(loop_, connectorFd_);

            auto weak = weak_from_this();

            connectorChannel_->setWriteCallback([weak]() {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->handleConnectWrite();
            });

            connectorChannel_->setErrorCallback([weak]() {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->handleConnectError();
            });

            connectorChannel_->setCloseCallback([weak]() {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->handleConnectError();
            });

            connectorChannel_->enableWriting();
            break;
        }

        default:
            reportConnectError(savedErrno, std::strerror(savedErrno));
            closeConnectorFd();
            state_.store(State::kDisconnected, std::memory_order_release);

            if (closeCompleteCallback_) {
                closeCompleteCallback_();
            }

            break;
    }
}

void TcpClient::disconnectInLoop() {
    loop_->assertInLoopThread();

    const State current = state_.load(std::memory_order_acquire);

    if (current == State::kDisconnected) {
        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }
        return;
    }

    if (current == State::kConnecting) {
        resetConnectorChannel();
        closeConnectorFd();

        state_.store(State::kDisconnected, std::memory_order_release);

        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }

        return;
    }

    state_.store(State::kDisconnecting, std::memory_order_release);

    TcpConnectionPtr conn;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn && conn->connected()) {
        conn->shutdown();
        return;
    }

    state_.store(State::kDisconnected, std::memory_order_release);

    if (closeCompleteCallback_) {
        closeCompleteCallback_();
    }
}

void TcpClient::stopInLoop() {
    loop_->assertInLoopThread();

    const State current = state_.load(std::memory_order_acquire);

    if (current == State::kDisconnected) {
        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }
        return;
    }

    if (current == State::kConnecting) {
        resetConnectorChannel();
        closeConnectorFd();

        state_.store(State::kDisconnected, std::memory_order_release);

        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }

        return;
    }

    state_.store(State::kDisconnecting, std::memory_order_release);

    TcpConnectionPtr conn;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn && conn->connected()) {
        conn->forceClose();
        return;
    }

    state_.store(State::kDisconnected, std::memory_order_release);

    if (closeCompleteCallback_) {
        closeCompleteCallback_();
    }
}

void TcpClient::handleConnectWrite() {
    loop_->assertInLoopThread();

    if (state_.load(std::memory_order_acquire) != State::kConnecting) {
        return;
    }

    const int sockfd = connectorFd_;

    resetConnectorChannel();

    const int errorCode = sockets::getSocketError(sockfd);

    if (errorCode != 0) {
        reportConnectError(errorCode, std::strerror(errorCode));
        closeConnectorFd();

        state_.store(State::kDisconnected, std::memory_order_release);

        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }

        return;
    }

    establishConnection(sockfd);
    connectorFd_ = -1;
}

void TcpClient::handleConnectError() {
    loop_->assertInLoopThread();

    if (connectorFd_ >= 0) {
        const int errorCode = sockets::getSocketError(connectorFd_);
        reportConnectError(errorCode, std::strerror(errorCode));
    }

    resetConnectorChannel();
    closeConnectorFd();

    state_.store(State::kDisconnected, std::memory_order_release);

    if (closeCompleteCallback_) {
        closeCompleteCallback_();
    }
}

void TcpClient::establishConnection(int sockfd) {
    loop_->assertInLoopThread();

    InetAddress localAddr(sockets::getLocalAddr(sockfd));
    InetAddress peerAddr(sockets::getPeerAddr(sockfd));

    std::string connName = name_ + "-" + peerAddr.toIpPort();

    auto conn = std::make_shared<TcpConnection>(loop_, std::move(connName), sockfd,
                                                localAddr, peerAddr);

    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    auto weak = weak_from_this();

    conn->setCloseCallback([weak](const TcpConnectionPtr& c) {
        auto self = weak.lock();

        if (self) {
            self->removeConnection(c);
            return;
        }

        /*
         * TcpClient 已经销毁时，不能访问 TcpClient。
         * 但 TcpConnection 仍然需要释放自己的 Channel/Poller 状态。
         */
        if (c) {
            c->connectDestroyed();
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }

    state_.store(State::kConnected, std::memory_order_release);

    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& connection) {
    auto weak = weak_from_this();

    loop_->queueInLoop([weak, connection]() {
        auto self = weak.lock();

        if (self) {
            self->removeConnectionInLoop(connection);
            return;
        }

        /*
         * TcpClient 已经销毁时，也必须清理 TcpConnection。
         */
        if (connection) {
            connection->connectDestroyed();
        }
    });
}

void TcpClient::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (connection_ == connection) {
            connection_.reset();
        }
    }

    state_.store(State::kDisconnected, std::memory_order_release);

    if (connection) {
        connection->connectDestroyed();
    }

    if (closeCompleteCallback_) {
        closeCompleteCallback_();
    }
}

void TcpClient::resetConnectorChannel() {
    if (!connectorChannel_) {
        return;
    }

    auto channel = connectorChannel_;
    connectorChannel_.reset();

    channel->disableAll();
    channel->remove();

    /*
     * 延迟释放 Channel，保证当前事件回调退出后再析构。
     */
    loop_->queueInLoop([channel]() { (void)channel; });
}

void TcpClient::closeConnectorFd() {
    if (connectorFd_ >= 0) {
        sockets::close(connectorFd_);
        connectorFd_ = -1;
    }
}

void TcpClient::reportConnectError(int errorCode, std::string message) {
    LOG_ERROR << "[TcpClient] connect failed, server=" << serverAddr_.toIpPort()
              << ", errno=" << errorCode << ", error=" << message;

    if (connectErrorCallback_) {
        connectErrorCallback_(errorCode, std::move(message));
    }
}

const char* TcpClient::stateToString(State state) noexcept {
    switch (state) {
        case State::kDisconnected:
            return "Disconnected";
        case State::kConnecting:
            return "Connecting";
        case State::kConnected:
            return "Connected";
        case State::kDisconnecting:
            return "Disconnecting";
        default:
            return "Unknown";
    }
}

}  // namespace novanet::net