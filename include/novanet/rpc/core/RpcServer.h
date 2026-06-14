#pragma once

#include <google/protobuf/service.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "novanet/base/Timestamp.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/net/TimerId.h"
#include "novanet/rpc/core/AiExecutor.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/core/ChatGenerateStreamHandler.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/RpcDispatcher.h"
#include "novanet/rpc/core/RpcServerStreamResponder.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamMethodInvoker.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"

namespace novanet::rpc {

class RpcServer final {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;

    struct Options {
        AiExecutor::Options aiExecutorOptions{};
        RpcServerStreamResponder::Options streamResponderOptions{};

        double streamTimeoutScanIntervalSeconds{5.0};
        double streamIdleTimeoutSeconds{60.0};
    };

    RpcServer(novanet::net::EventLoop* loop, const novanet::net::InetAddress& listenAddr,
              const std::string& name, AiProvider& aiProvider);

    RpcServer(novanet::net::EventLoop* loop, const novanet::net::InetAddress& listenAddr,
              const std::string& name, AiProvider& aiProvider, Options options);

    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    [[nodiscard]] bool registerService(google::protobuf::Service* service,
                                       std::string* errorText = nullptr);

    [[nodiscard]] std::size_t serviceCount() const noexcept;

    void setThreadNum(int numThreads);

    [[nodiscard]] bool start();

    void stop();

private:
    struct ConnectionContext {
        explicit ConnectionContext(std::shared_ptr<StreamManager> manager)
            : streamManager(std::move(manager)) {
        }

        RpcCodec codec;
        std::shared_ptr<StreamManager> streamManager;
        std::shared_ptr<RpcServerStreamResponder> responder;

        novanet::net::EventLoop* timerLoop{nullptr};
        novanet::net::TimerId streamTimeoutTimer;
        novanet::base::Timestamp lastSeen{novanet::base::Timestamp::now()};
    };

    using ConnectionKey = const novanet::net::TcpConnection*;

private:
    void onConnection(const TcpConnectionPtr& connection);

    void onMessage(const TcpConnectionPtr& connection, novanet::net::Buffer* buffer);

    [[nodiscard]] std::shared_ptr<ConnectionContext> createConnectionContext(
        const TcpConnectionPtr& connection);

    [[nodiscard]] std::shared_ptr<ConnectionContext> findConnectionContext(
        const TcpConnectionPtr& connection) const;

    void removeConnectionContext(const TcpConnectionPtr& connection);

    void closeConnectionSafely(const TcpConnectionPtr& connection, std::string reason);

    void handleRpcMessage(const TcpConnectionPtr& connection,
                          const std::shared_ptr<ConnectionContext>& context,
                          const RpcMessage& message);

    void sendImmediateResponses(const TcpConnectionPtr& connection,
                                ConnectionContext& context,
                                std::vector<RpcMessage>& responses);

    void sendOneImmediateResponse(const TcpConnectionPtr& connection,
                                  ConnectionContext& context, const RpcMessage& response);

    void startConnectionTimers(const TcpConnectionPtr& connection,
                               const std::shared_ptr<ConnectionContext>& context);

    void cancelConnectionTimers(const std::shared_ptr<ConnectionContext>& context);

    [[nodiscard]] static ConnectionKey connectionKey(
        const TcpConnectionPtr& connection) noexcept;

private:
    novanet::net::EventLoop* loop_{nullptr};
    novanet::net::TcpServer server_;

    Options options_;

    ServiceRegistry registry_;
    MethodInvoker invoker_;
    AiProvider& aiProvider_;
    AiExecutor aiExecutor_;
    StreamMethodInvoker streamInvoker_;
    ChatGenerateStreamHandler chatStreamHandler_;
    RpcDispatcher dispatcher_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex contextsMutex_;
    std::unordered_map<ConnectionKey, std::shared_ptr<ConnectionContext>> contexts_;
};

}  // namespace novanet::rpc
