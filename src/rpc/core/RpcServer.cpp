#include "novanet/rpc/core/RpcServer.h"

#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/base/Timestamp.h"

namespace novanet::rpc {

using novanet::base::Timestamp;

RpcServer::RpcServer(novanet::net::EventLoop* loop,
                     const novanet::net::InetAddress& listenAddr, const std::string& name,
                     AiProvider& aiProvider)
    : RpcServer(loop, listenAddr, name, aiProvider, Options{}) {
}

RpcServer::RpcServer(novanet::net::EventLoop* loop,
                     const novanet::net::InetAddress& listenAddr, const std::string& name,
                     AiProvider& aiProvider, Options options)
    : loop_(loop),
      server_(loop, listenAddr, name),
      options_(std::move(options)),
      aiProvider_(aiProvider),
      aiExecutor_(options_.aiExecutorOptions),
      streamInvoker_(registry_),
      chatStreamHandler_(aiProvider_, aiExecutor_),
      dispatcher_(registry_, invoker_, streamInvoker_) {
    std::string streamHandlerError;
    if (!streamInvoker_.registerHandler(&chatStreamHandler_,
                                        &streamHandlerError)) {
        LOG_ERROR << "[RpcServer] register chat stream handler failed: "
                  << streamHandlerError;
    }

    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& connection) { this->onConnection(connection); });

    server_.setMessageCallback(
        [this](const TcpConnectionPtr& connection, novanet::net::Buffer* buffer) {
            this->onMessage(connection, buffer);
        });

    aiExecutor_.setErrorHandler([](std::string error) {
        LOG_ERROR << "[RpcServer] AiExecutor error: " << error;
    });
}

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::registerService(google::protobuf::Service* service,
                                std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    if (service == nullptr) {
        if (errorText != nullptr) {
            *errorText = "service is null";
        }
        LOG_ERROR << "[RpcServer] registerService failed: service is null";
        return false;
    }

    if (started_.load(std::memory_order_acquire)) {
        if (errorText != nullptr) {
            *errorText = "server already started";
        }
        LOG_ERROR << "[RpcServer] registerService failed: server already started";
        return false;
    }

    return registry_.registerService(service, errorText);
}

std::size_t RpcServer::serviceCount() const noexcept {
    return registry_.serviceCount();
}

void RpcServer::setThreadNum(int numThreads) {
    if (started_.load(std::memory_order_acquire)) {
        LOG_WARN << "[RpcServer] setThreadNum ignored after start";
        return;
    }

    server_.setThreadNum(numThreads);
}

bool RpcServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return true;
    }

    stopping_.store(false, std::memory_order_release);

    if (!aiExecutor_.start()) {
        LOG_ERROR << "[RpcServer] failed to start AiExecutor";
        stopping_.store(true, std::memory_order_release);
        started_.store(false, std::memory_order_release);
        return false;
    }

    LOG_INFO << "[RpcServer] starting TcpServer";
    server_.start();
    return true;
}

void RpcServer::stop() {
    const bool wasStarted = started_.exchange(false, std::memory_order_acq_rel);
    if (!wasStarted) {
        return;
    }

    stopping_.store(true, std::memory_order_release);

    std::vector<std::shared_ptr<ConnectionContext>> contexts;

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        contexts.reserve(contexts_.size());

        for (auto& item : contexts_) {
            cancelConnectionTimers(item.second);
            contexts.push_back(item.second);
        }

        contexts_.clear();
    }

    for (auto& context : contexts) {
        if (context && context->responder) {
            context->responder->markConnectionClosed("RpcServer stopped");
        }
    }

    aiExecutor_.stop(AiExecutor::StopMode::kDiscardPending);

    LOG_INFO << "[RpcServer] stopped";
}

void RpcServer::onConnection(const TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }

    if (stopping_.load(std::memory_order_acquire)) {
        closeConnectionSafely(connection, "server stopping");
        return;
    }

    if (connection->connected()) {
        auto context = createConnectionContext(connection);
        if (!context) {
            LOG_ERROR << "[RpcServer] create connection context failed";
            closeConnectionSafely(connection, "create context failed");
            return;
        }

        LOG_INFO << "[RpcServer] connection up: " << connection->name();
        return;
    }

    LOG_INFO << "[RpcServer] connection down: " << connection->name();
    removeConnectionContext(connection);
}

void RpcServer::onMessage(const TcpConnectionPtr& connection,
                          novanet::net::Buffer* buffer) {
    if (!connection || buffer == nullptr) {
        return;
    }

    auto context = findConnectionContext(connection);
    if (!context) {
        LOG_ERROR << "[RpcServer] missing context, connection=" << connection->name();
        closeConnectionSafely(connection, "missing context");
        return;
    }

    context->lastSeen = Timestamp::now();

    while (true) {
        RpcMessage message;
        const RpcCodec::DecodeStatus status = context->codec.tryDecode(*buffer, message);

        if (status == RpcCodec::DecodeStatus::kNeedMore) {
            break;
        }

        if (status == RpcCodec::DecodeStatus::kInvalid) {
            LOG_ERROR << "[RpcServer] decode error, close connection="
                      << connection->name();
            closeConnectionSafely(connection, "decode error");
            return;
        }

        if (status != RpcCodec::DecodeStatus::kOk || !message.valid()) {
            LOG_ERROR << "[RpcServer] invalid decoded message, close connection="
                      << connection->name();
            closeConnectionSafely(connection, "invalid message");
            return;
        }

        handleRpcMessage(connection, context, message);
    }
}

std::shared_ptr<RpcServer::ConnectionContext> RpcServer::createConnectionContext(
    const TcpConnectionPtr& connection) {
    auto streamManager = std::make_shared<StreamManager>();
    auto context = std::make_shared<ConnectionContext>(streamManager);

    auto responder = RpcServerStreamResponder::create(connection, streamManager,
                                                      options_.streamResponderOptions);

    if (!responder) {
        return nullptr;
    }

    context->responder = std::move(responder);

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        contexts_[connectionKey(connection)] = context;
    }

    startConnectionTimers(connection, context);
    return context;
}

std::shared_ptr<RpcServer::ConnectionContext> RpcServer::findConnectionContext(
    const TcpConnectionPtr& connection) const {
    const ConnectionKey key = connectionKey(connection);
    if (key == nullptr) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(contextsMutex_);
    auto it = contexts_.find(key);
    if (it == contexts_.end()) {
        return nullptr;
    }

    return it->second;
}

void RpcServer::removeConnectionContext(const TcpConnectionPtr& connection) {
    std::shared_ptr<ConnectionContext> context;

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = contexts_.find(connectionKey(connection));
        if (it == contexts_.end()) {
            return;
        }

        context = it->second;
        cancelConnectionTimers(context);
        contexts_.erase(it);
    }

    if (context && context->responder) {
        context->responder->markConnectionClosed("connection closed");
    }
}

void RpcServer::closeConnectionSafely(const TcpConnectionPtr& connection,
                                      std::string reason) {
    if (!connection) {
        return;
    }

    removeConnectionContext(connection);

    if (connection->connected()) {
        LOG_WARN << "[RpcServer] shutdown connection=" << connection->name()
                 << ", reason=" << reason;
        connection->shutdown();
    }
}

void RpcServer::handleRpcMessage(const TcpConnectionPtr& connection,
                                 const std::shared_ptr<ConnectionContext>& context,
                                 const RpcMessage& message) {
    std::vector<RpcMessage> immediateResponses;

    const bool ok = dispatcher_.dispatch(message, *context->streamManager,
                                         immediateResponses, context->responder);

    sendImmediateResponses(connection, *context, immediateResponses);

    if (!ok && immediateResponses.empty()) {
        closeConnectionSafely(connection, "dispatcher failed without response");
    }
}

void RpcServer::sendImmediateResponses(const TcpConnectionPtr& connection,
                                       ConnectionContext& context,
                                       std::vector<RpcMessage>& responses) {
    for (const auto& response : responses) {
        sendOneImmediateResponse(connection, context, response);
    }
}

void RpcServer::sendOneImmediateResponse(const TcpConnectionPtr& connection,
                                         ConnectionContext& context,
                                         const RpcMessage& response) {
    if (!connection || !connection->connected()) {
        return;
    }

    novanet::net::Buffer out;
    if (!context.codec.encode(response, out)) {
        LOG_ERROR << "[RpcServer] encode immediate response failed";
        closeConnectionSafely(connection, "encode immediate response failed");
        return;
    }

    connection->send(out.retrieveAllAsString());
}

void RpcServer::startConnectionTimers(const TcpConnectionPtr& connection,
                                      const std::shared_ptr<ConnectionContext>& context) {
    if (!connection || !context) {
        return;
    }

    auto* loop = connection->getLoop();
    if (loop == nullptr) {
        LOG_ERROR << "[RpcServer] connection loop is null";
        return;
    }

    std::weak_ptr<ConnectionContext> weakContext = context;
    std::weak_ptr<novanet::net::TcpConnection> weakConnection = connection;
    const double streamIdleTimeoutSeconds = options_.streamIdleTimeoutSeconds;

    context->timerLoop = loop;
    context->streamTimeoutTimer = loop->runEvery(
        options_.streamTimeoutScanIntervalSeconds,
        [weakContext, weakConnection, streamIdleTimeoutSeconds]() {
            auto context = weakContext.lock();
            auto connection = weakConnection.lock();

            if (!context || !connection || !connection->connected()) {
                return;
            }

            const Timestamp now = Timestamp::now();

            auto expiredStreams = context->streamManager->timeoutStreamsWithInfo(
                now, streamIdleTimeoutSeconds, "server stream idle timeout");

            for (const auto& stream : expiredStreams) {
                LOG_WARN << "[RpcServer] stream timeout, connection="
                         << connection->name() << ", streamId=" << stream.streamId;

                if (context->responder) {
                    (void)context->responder->sendEnd(
                        stream.streamId, stream.requestId, meta::RPC_TIMEOUT,
                        "server stream idle timeout");
                }
            }
        });
}

void RpcServer::cancelConnectionTimers(
    const std::shared_ptr<ConnectionContext>& context) {
    if (!context) {
        return;
    }

    auto* loop = context->timerLoop;
    if (loop == nullptr) {
        return;
    }

    if (context->streamTimeoutTimer.valid()) {
        loop->cancel(context->streamTimeoutTimer);
        context->streamTimeoutTimer = novanet::net::TimerId{};
    }
    context->timerLoop = nullptr;
}

RpcServer::ConnectionKey RpcServer::connectionKey(
    const TcpConnectionPtr& connection) noexcept {
    return connection ? connection.get() : nullptr;
}

}  // namespace novanet::rpc
