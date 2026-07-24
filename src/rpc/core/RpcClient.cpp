#include "novanet/rpc/core/RpcClient.h"

#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"

namespace novanet::rpc {

namespace {

const RpcClient::MetadataMap kEmptyMetadata{};

}  // namespace

RpcClient::RpcClient(const novanet::net::InetAddress& serverAddr, std::string name)
    : RpcClient(serverAddr, std::move(name), Options{}) {
}

RpcClient::RpcClient(const novanet::net::InetAddress& serverAddr, std::string name,
                     Options options)
    : serverAddr_(serverAddr),
      name_(std::move(name)),
      options_(std::move(options)),
      loopThread_() {
}

RpcClient::~RpcClient() {
    disconnect();
}

std::weak_ptr<RpcClient> RpcClient::weakSelfOrDie() {
    std::weak_ptr<RpcClient> weakSelf = weak_from_this();

    if (weakSelf.expired()) {
        LOG_FATAL << "[RpcClient] must be managed by std::shared_ptr, name="
                  << name_;
    }

    return weakSelf;
}

bool RpcClient::connect(std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (state_ == State::kConnected) {
            return true;
        }

        if (state_ == State::kConnecting) {
            cv_.wait(lock, [this]() { return state_ != State::kConnecting; });

            if (state_ == State::kConnected) {
                return true;
            }

            if (errorText != nullptr) {
                *errorText = lastError_.empty() ? "connect failed" : lastError_;
            }

            return false;
        }

        if (state_ == State::kClosing) {
            if (errorText != nullptr) {
                *errorText = "RpcClient is closing";
            }
            return false;
        }

        if (state_ == State::kClosed) {
            if (errorText != nullptr) {
                *errorText = "RpcClient already closed";
            }
            return false;
        }

        state_ = State::kConnecting;
        closeComplete_ = false;
        disconnectComplete_ = false;
        lastError_.clear();
    }

    novanet::net::EventLoop* loop = loopThread_.startLoop();
    if (loop == nullptr) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = nullptr;
            state_ = State::kClosed;
            closeComplete_ = true;
            disconnectComplete_ = true;
            lastError_ = "failed to start client EventLoop";
        }

        cv_.notify_all();

        if (errorText != nullptr) {
            *errorText = "failed to start client EventLoop";
        }

        LOG_ERROR << "[RpcClient] failed to start client EventLoop, name=" << name_;

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != State::kConnecting) {
            lastError_ = "connect cancelled before TcpClient creation";

            cv_.notify_all();

            if (errorText != nullptr) {
                *errorText = lastError_;
            }

            return false;
        }

        loop_ = loop;
    }

    auto weakSelf = weakSelfOrDie();

    auto tcpClient =
        std::make_shared<novanet::net::TcpClient>(loop, serverAddr_, name_);

    tcpClient->setConnectionCallback(
        [weakSelf](const novanet::net::TcpConnection::TcpConnectionPtr& conn) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }

            self->handleTcpConnection(conn);
        });

    tcpClient->setMessageCallback(
        [weakSelf](const novanet::net::TcpConnection::TcpConnectionPtr& conn,
                   novanet::net::Buffer* buffer) {
            auto self = weakSelf.lock();
            if (!self) {
                if (buffer != nullptr) {
                    buffer->retrieveAll();
                }
                return;
            }

            self->handleTcpMessage(conn, buffer);
        });

    tcpClient->setConnectErrorCallback([weakSelf](int errorCode, std::string error) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }

        self->handleTcpConnectError(errorCode, std::move(error));
    });

    tcpClient->setCloseCompleteCallback([this]() {
        handleTcpCloseComplete();
    });

    std::shared_ptr<novanet::net::TcpClient> clientToConnect;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != State::kConnecting) {
            lastError_ = "connect cancelled before tcp connect";

            cv_.notify_all();

            if (errorText != nullptr) {
                *errorText = lastError_;
            }

            return false;
        }

        tcpClient_ = std::move(tcpClient);
        clientToConnect = tcpClient_;
    }

    clientToConnect->connect();

    std::unique_lock<std::mutex> lock(mutex_);

    const bool done = cv_.wait_for(lock, options_.connectTimeout, [this]() {
        return state_ != State::kConnecting;
    });

    if (!done) {
        lastError_ = "connect timeout";
        state_ = State::kClosing;

        lock.unlock();

        LOG_ERROR << "[RpcClient] connect timeout, name=" << name_;

        disconnect();

        if (errorText != nullptr) {
            *errorText = "connect timeout";
        }

        return false;
    }

    if (state_ == State::kConnected) {
        LOG_INFO << "[RpcClient] connected, name=" << name_;
        return true;
    }

    const std::string error = lastError_.empty() ? "connect failed" : lastError_;

    const bool shouldCleanup = state_ == State::kClosed || state_ == State::kClosing;

    lock.unlock();

    if (shouldCleanup) {
        disconnect();
    }

    if (errorText != nullptr) {
        *errorText = error;
    }

    LOG_ERROR << "[RpcClient] connect failed, name=" << name_ << ", error=" << error;

    return false;
}

void RpcClient::disconnect() {
    std::lock_guard<std::mutex> disconnectLock(disconnectMutex_);

    std::shared_ptr<novanet::net::TcpClient> clientToStop;
    std::shared_ptr<novanet::net::TcpClient> clientToRelease;
    std::shared_ptr<RpcChannel> channelToClose;

    bool shouldWaitClose = false;
    novanet::net::EventLoop* loop = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == State::kClosed && disconnectComplete_) {
            return;
        }

        loop = loop_;

        if (loop != nullptr && loop->isInLoopThread()) {
            LOG_FATAL << "[RpcClient] synchronous disconnect called from EventLoop "
                         "thread, name="
                      << name_;
            return;
        }

        if (state_ == State::kIdle) {
            state_ = State::kClosed;
            closeComplete_ = true;
            disconnectComplete_ = true;

            channelToClose = std::move(channel_);
            clientToRelease = std::move(tcpClient_);

            lastError_ = "RpcClient disconnected";
            cv_.notify_all();
        } else {
            state_ = State::kClosing;
            disconnectComplete_ = false;

            if (tcpClient_ != nullptr) {
                clientToStop = tcpClient_;
                shouldWaitClose = !closeComplete_;
            } else {
                closeComplete_ = true;
            }
        }
    }

    cv_.notify_all();

    if (clientToStop != nullptr) {
        clientToStop->stop();
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shouldWaitClose) {
            cv_.wait(lock, [this]() { return closeComplete_; });
        }

        if (!channelToClose) {
            channelToClose = std::move(channel_);
        }

        if (!clientToRelease) {
            clientToRelease = std::move(tcpClient_);
        }

    }

    if (channelToClose) {
        channelToClose->onConnectionClosed("RpcClient disconnected");
    }

    loopThread_.stopAndJoin();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
        state_ = State::kClosed;
        closeComplete_ = true;
        disconnectComplete_ = true;
        lastError_ = "RpcClient disconnected";
    }

    clientToRelease.reset();
    clientToStop.reset();
    channelToClose.reset();

    cv_.notify_all();

    LOG_INFO << "[RpcClient] disconnected, name=" << name_;
}

bool RpcClient::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return state_ == State::kConnected && channel_ != nullptr;
}

RpcStatus RpcClient::callUnary(const std::string& serviceName,
                               const std::string& methodName,
                               const google::protobuf::Message& request,
                               google::protobuf::Message* response,
                               std::chrono::milliseconds timeout) {
    return callUnary(serviceName, methodName, request, response, timeout,
                     kEmptyMetadata);
}

RpcStatus RpcClient::callUnary(const std::string& serviceName,
                               const std::string& methodName,
                               const google::protobuf::Message& request,
                               google::protobuf::Message* response,
                               std::chrono::milliseconds timeout,
                               const MetadataMap& metadata) {
    return callUnary(serviceName, methodName, request, response, timeout, metadata,
                     nullptr);
}

RpcStatus RpcClient::callUnary(const std::string& serviceName,
                               const std::string& methodName,
                               const google::protobuf::Message& request,
                               google::protobuf::Message* response,
                               std::chrono::milliseconds timeout,
                               const MetadataMap& metadata,
                               std::uint64_t* requestIdOut) {
    auto channel = channelSnapshot();

    if (!channel) {
        return RpcStatus::failure(meta::RPC_CONNECTION_CLOSED,
                                  "RpcClient is not connected");
    }

    return channel->callUnary(serviceName, methodName, request, response, timeout,
                              metadata, requestIdOut);
}

RpcChannel::StreamHandle RpcClient::openStream(
    const std::string& serviceName, const std::string& methodName,
    const google::protobuf::Message& request,
    RpcChannel::StreamCallbacks callbacks) {
    return openStream(serviceName, methodName, request, std::move(callbacks),
                      kEmptyMetadata);
}

RpcChannel::StreamHandle RpcClient::openStream(
    const std::string& serviceName, const std::string& methodName,
    const google::protobuf::Message& request, RpcChannel::StreamCallbacks callbacks,
    const MetadataMap& metadata) {
    auto channel = channelSnapshot();

    if (!channel) {
        return RpcChannel::StreamHandle{0, 0, false, "RpcClient is not connected"};
    }

    return channel->openStream(serviceName, methodName, request,
                               std::move(callbacks), metadata);
}

bool RpcClient::cancelStream(std::uint32_t streamId, std::string reason) {
    auto channel = channelSnapshot();

    if (!channel) {
        return false;
    }

    return channel->cancelStream(streamId, std::move(reason));
}

bool RpcClient::sendHeartbeatPing() {
    auto channel = channelSnapshot();

    if (!channel) {
        return false;
    }

    return channel->sendHeartbeatPing();
}

std::shared_ptr<RpcChannel> RpcClient::channelSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channel_;
}

void RpcClient::handleTcpConnection(
    const novanet::net::TcpConnection::TcpConnectionPtr& conn) {
    if (conn && conn->connected()) {
        auto channel = std::make_shared<RpcChannel>(conn, options_.channelOptions);

        bool accepted = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_ == State::kConnecting) {
                channel_ = channel;
                accepted = true;
            }
        }

        if (!accepted) {
            channel->onConnectionClosed("RpcClient not accepting connection");

            if (conn->connected()) {
                conn->shutdown();
            }

            return;
        }

        if (options_.startHeartbeatTimers) {
            channel->startTimers();
        }

        notifyConnected();
        return;
    }

    std::shared_ptr<RpcChannel> channel;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel = channel_;
        channel_.reset();
    }

    if (channel) {
        channel->onConnectionClosed("tcp connection closed");
    }

    notifyClosed("tcp connection closed");
}

void RpcClient::handleTcpMessage(
    const novanet::net::TcpConnection::TcpConnectionPtr& conn,
    novanet::net::Buffer* buffer) {
    auto channel = channelSnapshot();

    if (channel) {
        channel->onMessage(conn, buffer);
        return;
    }

    if (buffer != nullptr) {
        buffer->retrieveAll();
    }
}

void RpcClient::handleTcpConnectError(int errorCode, std::string error) {
    static_cast<void>(errorCode);
    notifyConnectError(std::move(error));
}

void RpcClient::handleTcpCloseComplete() {
    notifyCloseComplete();
}

void RpcClient::notifyConnected() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == State::kClosing || state_ == State::kClosed) {
            lastError_ = "connected after closing";
            return;
        }

        state_ = State::kConnected;
        closeComplete_ = false;
        lastError_.clear();
    }

    cv_.notify_all();
}

void RpcClient::notifyClosed(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == State::kConnected || state_ == State::kConnecting) {
            state_ = State::kClosed;
        }

        lastError_ = std::move(reason);
    }

    cv_.notify_all();
}

void RpcClient::notifyCloseComplete() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        closeComplete_ = true;
    }

    cv_.notify_all();
}

void RpcClient::notifyConnectError(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (reason.empty()) {
            reason = "connect error";
        }

        state_ = State::kClosed;
        closeComplete_ = true;
        lastError_ = std::move(reason);
    }

    cv_.notify_all();
}

const char* RpcClient::stateToString(State state) noexcept {
    switch (state) {
        case State::kIdle:
            return "Idle";
        case State::kConnecting:
            return "Connecting";
        case State::kConnected:
            return "Connected";
        case State::kClosing:
            return "Closing";
        case State::kClosed:
            return "Closed";
        default:
            return "Unknown";
    }
}

}  // namespace novanet::rpc
