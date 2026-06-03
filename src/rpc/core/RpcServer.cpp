#include "novanet/rpc/core/RpcServer.h"

#include <utility>

#include "novanet/base/Logger.h"

namespace novanet::rpc {

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
      dispatcher_(registry_, invoker_, aiProvider_, aiExecutor_) {
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

bool RpcServer::registerService(google::protobuf::Service* service) {
    if (service == nullptr) {
        LOG_ERROR << "[RpcServer] registerService failed: service is null";
        return false;
    }

    if (started_.load(std::memory_order_acquire)) {
        LOG_ERROR << "[RpcServer] registerService failed: server already started";
        return false;
    }

    return registry_.registerService(service);
}

void RpcServer::setThreadNum(int numThreads) {
    if (started_.load(std::memory_order_acquire)) {
        LOG_ERROR << "[RpcServer] setThreadNum ignored: server already started";
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
        started_.store(false, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
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
            contexts.push_back(item.second);
        }

        contexts_.clear();
    }

    /*
     * 先让所有 responder 进入 closed 状态。
     * 这样 worker 里的 shouldStop 会尽快返回 cancelled。
     */
    for (auto& context : contexts) {
        if (context && context->responder) {
            context->responder->markConnectionClosed();
        }
    }

    /*
     * 停止 AI executor。
     *
     * kDiscardPending:
     * - 丢弃尚未开始的 AI 任务；
     * - 已经运行的任务依赖 shouldStop 尽快退出。
     */
    aiExecutor_.stop(AiExecutor::StopMode::kDiscardPending);

    /*
     * 如果你的 TcpServer 已经实现 stop()，可以在这里补：
     *
     *   server_.stop();
     *
     * 当前不强行调用，避免与你现有 Phase 3 TcpServer 接口不一致。
     */
    LOG_INFO << "[RpcServer] stopped";
}

void RpcServer::onConnection(const TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }

    if (stopping_.load(std::memory_order_acquire)) {
        closeConnectionSafely(connection);
        return;
    }

    if (connection->connected()) {
        auto context = createConnectionContext(connection);
        if (!context) {
            LOG_ERROR << "[RpcServer] failed to create connection context";
            closeConnectionSafely(connection);
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

    if (stopping_.load(std::memory_order_acquire)) {
        closeConnectionSafely(connection);
        return;
    }

    auto context = findConnectionContext(connection);
    if (!context) {
        LOG_ERROR << "[RpcServer] missing connection context: " << connection->name();
        closeConnectionSafely(connection);
        return;
    }

    while (true) {
        RpcMessage message;

        const RpcCodec::DecodeStatus status = context->codec.tryDecode(*buffer, message);

        if (status == RpcCodec::DecodeStatus::kNeedMore) {
            break;
        }

        if (status == RpcCodec::DecodeStatus::kInvalid) {
            LOG_ERROR << "[RpcServer] RpcCodec decode error, close connection: "
                      << connection->name();
            closeConnectionSafely(connection);
            return;
        }

        if (status != RpcCodec::DecodeStatus::kOk) {
            LOG_ERROR << "[RpcServer] unknown RpcCodec decode status, close connection: "
                      << connection->name();
            closeConnectionSafely(connection);
            return;
        }

        if (!message.valid()) {
            LOG_ERROR << "[RpcServer] decoded invalid RpcMessage, close connection: "
                      << connection->name();
            closeConnectionSafely(connection);
            return;
        }

        handleRpcMessage(connection, context, message);
    }
}

std::shared_ptr<RpcServer::ConnectionContext> RpcServer::createConnectionContext(
    const TcpConnectionPtr& connection) {
    if (!connection) {
        return nullptr;
    }

    auto streamManager = std::make_shared<StreamManager>();
    auto context = std::make_shared<ConnectionContext>(streamManager);

    auto responder = RpcServerStreamResponder::create(connection, streamManager,
                                                      options_.streamResponderOptions);

    if (!responder) {
        return nullptr;
    }

    context->responder = std::move(responder);

    const ConnectionKey key = connectionKey(connection);
    if (key == nullptr) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);

        /*
         * 防御：同一个 connection 重复 up 时，先关闭旧上下文。
         */
        auto old = contexts_.find(key);
        if (old != contexts_.end()) {
            if (old->second && old->second->responder) {
                old->second->responder->markConnectionClosed();
            }
            contexts_.erase(old);
        }

        contexts_.emplace(key, context);
    }

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
    const ConnectionKey key = connectionKey(connection);
    if (key == nullptr) {
        return;
    }

    std::shared_ptr<ConnectionContext> context;

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);

        auto it = contexts_.find(key);
        if (it == contexts_.end()) {
            return;
        }

        context = it->second;
        contexts_.erase(it);
    }

    if (context && context->responder) {
        context->responder->markConnectionClosed();
    }
}

void RpcServer::closeConnectionSafely(const TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }

    removeConnectionContext(connection);

    if (connection->connected()) {
        connection->shutdown();
    }
}

void RpcServer::handleRpcMessage(const TcpConnectionPtr& connection,
                                 const std::shared_ptr<ConnectionContext>& context,
                                 const RpcMessage& message) {
    if (!connection || !context || !context->streamManager) {
        return;
    }

    std::vector<RpcMessage> immediateResponses;

    /*
     * 核心：
     * - StreamManager 是当前连接自己的；
     * - responder 是当前连接自己的；
     * - STREAM_OPEN 成功后不会同步生成 DATA；
     * - 后续 DATA/END 由 AiExecutor worker + responder 异步发送。
     */
    const bool ok = dispatcher_.dispatch(message, *context->streamManager,
                                         immediateResponses, context->responder);

    /*
     * immediateResponses 包括：
     * - UNARY_RESPONSE
     * - HEARTBEAT_PONG
     * - immediate ERROR_FRAME
     */
    sendImmediateResponses(connection, *context, immediateResponses);

    if (!ok && immediateResponses.empty()) {
        LOG_ERROR << "[RpcServer] dispatcher failed without response, close connection: "
                  << connection->name();
        closeConnectionSafely(connection);
    }
}

void RpcServer::sendImmediateResponses(const TcpConnectionPtr& connection,
                                       ConnectionContext& context,
                                       std::vector<RpcMessage>& responses) {
    if (!connection || responses.empty()) {
        return;
    }

    /*
     * onMessage 在连接所属 EventLoop 中执行。
     * immediate response 可以直接 encode/send。
     */
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
        LOG_ERROR << "[RpcServer] failed to encode immediate response, close connection: "
                  << connection->name();
        closeConnectionSafely(connection);
        return;
    }

    connection->send(out.retrieveAllAsString());
}

RpcServer::ConnectionKey RpcServer::connectionKey(
    const TcpConnectionPtr& connection) noexcept {
    if (!connection) {
        return nullptr;
    }

    return connection.get();
}

}  // namespace novanet::rpc