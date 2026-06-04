#include "novanet/rpc/core/RpcClient.h"

#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"

namespace novanet::rpc {

RpcClient::RpcClient(const novanet::net::InetAddress& serverAddr,
                     std::string name)
    : RpcClient(serverAddr, std::move(name), Options{}) {
}

RpcClient::RpcClient(const novanet::net::InetAddress& serverAddr,
                     std::string name, Options options)
    : serverAddr_(serverAddr),
      name_(std::move(name)),
      options_(std::move(options)),
      loopThread_() {
}

RpcClient::~RpcClient() {
    disconnect();
}

bool RpcClient::connect(std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            return true;
        }

        if (started_) {
            if (errorText != nullptr) {
                *errorText = "RpcClient already started";
            }
            return false;
        }

        started_ = true;
        connectFinished_ = false;
        closeComplete_ = false;
        lastError_.clear();
    }

    loop_ = loopThread_.startLoop();
    if (loop_ == nullptr) {
        if (errorText != nullptr) {
            *errorText = "failed to start client EventLoop";
        }
        return false;
    }

    tcpClient_ = std::make_unique<novanet::net::TcpClient>(loop_, serverAddr_,
                                                           name_);

    tcpClient_->setConnectionCallback(
        [this](const novanet::net::TcpConnection::TcpConnectionPtr& conn) {
            if (conn && conn->connected()) {
                auto channel =
                    std::make_shared<RpcChannel>(conn, options_.channelOptions);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    channel_ = channel;
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
        });

    tcpClient_->setMessageCallback(
        [this](const novanet::net::TcpConnection::TcpConnectionPtr& conn,
               novanet::net::Buffer* buffer) {
            auto channel = channelSnapshot();
            if (channel) {
                channel->onMessage(conn, buffer);
            } else if (buffer != nullptr) {
                buffer->retrieveAll();
            }
        });

    tcpClient_->setConnectErrorCallback(
        [this](int, std::string error) { notifyConnectError(std::move(error)); });
    tcpClient_->setCloseCompleteCallback(
        [this]() { notifyCloseComplete(); });

    tcpClient_->connect();

    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = cv_.wait_for(lock, options_.connectTimeout, [this]() {
        return connectFinished_;
    });

    if (!done) {
        lastError_ = "connect timeout";
        lock.unlock();
        disconnect();
        if (errorText != nullptr) {
            *errorText = "connect timeout";
        }
        return false;
    }

    if (!connected_) {
        if (errorText != nullptr) {
            *errorText = lastError_.empty() ? "connect failed" : lastError_;
        }
        return false;
    }

    return true;
}

void RpcClient::disconnect() {
    if (tcpClient_) {
        tcpClient_->stop();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(1000), [this]() {
        return !connected_ && closeComplete_;
    });

    channel_.reset();
    connected_ = false;
}

bool RpcClient::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

RpcStatus RpcClient::callUnary(const std::string& serviceName,
                               const std::string& methodName,
                               const google::protobuf::Message& request,
                               google::protobuf::Message* response,
                               std::chrono::milliseconds timeout) {
    auto channel = channelSnapshot();
    if (!channel) {
        return RpcStatus::failure(meta::RPC_CONNECTION_CLOSED,
                                  "RpcClient is not connected");
    }

    return channel->callUnary(serviceName, methodName, request, response,
                              timeout);
}

RpcChannel::StreamHandle RpcClient::openStream(
    const std::string& serviceName, const std::string& methodName,
    const google::protobuf::Message& request,
    RpcChannel::StreamCallbacks callbacks) {
    auto channel = channelSnapshot();
    if (!channel) {
        return RpcChannel::StreamHandle{0, 0, false,
                                        "RpcClient is not connected"};
    }

    return channel->openStream(serviceName, methodName, request,
                               std::move(callbacks));
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

void RpcClient::notifyConnected() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = true;
        connectFinished_ = true;
        closeComplete_ = false;
        lastError_.clear();
    }
    cv_.notify_all();
}

void RpcClient::notifyClosed(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        lastError_ = std::move(reason);
        connectFinished_ = true;
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
        connected_ = false;
        closeComplete_ = true;
        lastError_ = std::move(reason);
        connectFinished_ = true;
    }
    cv_.notify_all();
}

}  // namespace novanet::rpc
