#include "novanet/net/TcpClient.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Channel.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/SocketsOps.h"

namespace novanet::net {

namespace {

void defaultConnectionCallback(const TcpConnection::TcpConnectionPtr& conn) {
    LOG_INFO << "TcpClient connection " << conn->name() << " is "
             << (conn->connected() ? "UP" : "DOWN");
}

void defaultMessageCallback(const TcpConnection::TcpConnectionPtr&,
                            Buffer* buffer) {
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
    stop();
}

void TcpClient::connect() {
    loop_->runInLoop([this]() { this->connectInLoop(); });
}

void TcpClient::disconnect() {
    loop_->runInLoop([this]() { this->disconnectInLoop(); });
}

void TcpClient::stop() {
    loop_->runInLoop([this]() { this->stopInLoop(); });
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

    if (state_.load(std::memory_order_acquire) != State::kDisconnected) {
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
        case EISCONN:
            state_.store(State::kConnecting, std::memory_order_release);
            connectorChannel_ = std::make_shared<Channel>(loop_, connectorFd_);
            connectorChannel_->setWriteCallback(
                [this]() { this->handleConnectWrite(); });
            connectorChannel_->setErrorCallback(
                [this]() { this->handleConnectError(); });
            connectorChannel_->enableWriting();
            break;

        default:
            reportConnectError(savedErrno, std::strerror(savedErrno));
            closeConnectorFd();
            state_.store(State::kDisconnected, std::memory_order_release);
            break;
    }
}

void TcpClient::disconnectInLoop() {
    loop_->assertInLoopThread();

    if (state_.load(std::memory_order_acquire) == State::kConnecting) {
        resetConnectorChannel();
        closeConnectorFd();
        state_.store(State::kDisconnected, std::memory_order_release);
        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }
        return;
    }

    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn && conn->connected()) {
        conn->shutdown();
    }
}

void TcpClient::stopInLoop() {
    loop_->assertInLoopThread();

    if (state_.load(std::memory_order_acquire) == State::kConnecting) {
        resetConnectorChannel();
        closeConnectorFd();
        state_.store(State::kDisconnected, std::memory_order_release);
        if (closeCompleteCallback_) {
            closeCompleteCallback_();
        }
        return;
    }

    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn && conn->connected()) {
        conn->forceClose();
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
    conn->setCloseCallback([this](const TcpConnectionPtr& c) {
        this->removeConnection(c);
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }

    state_.store(State::kConnected, std::memory_order_release);
    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& connection) {
    loop_->queueInLoop([this, connection]() {
        this->removeConnectionInLoop(connection);
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

    loop_->queueInLoop([channel]() {
        (void)channel;
    });
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

}  // namespace novanet::net
