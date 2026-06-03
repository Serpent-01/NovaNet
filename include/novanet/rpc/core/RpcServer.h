#pragma once

#include <google/protobuf/service.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/rpc/core/AiExecutor.h"
#include "novanet/rpc/core/AiProvider.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/RpcDispatcher.h"
#include "novanet/rpc/core/RpcServerStreamResponder.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"

namespace novanet::rpc {

/*
 * RpcServer 是 NovaNet 服务端 RPC 入口。
 *
 * Phase 4 企业级职责：
 * - 接入 TcpServer；
 * - 使用 RpcCodec 做 Buffer <-> RpcMessage；
 * - 使用 RpcDispatcher 做 RPC 语义分发；
 * - immediateResponses 只发送 unary / heartbeat / immediate error；
 * - STREAM_OPEN 后续 STREAM_DATA / STREAM_END 由 StreamResponder 异步发送；
 * - 每个连接一个 StreamManager，避免不同连接的 stream_id 冲突；
 * - 每个连接一个 RpcServerStreamResponder；
 * - 连接关闭时 cancelAll streams，让 AiProvider::shouldStop 停止 worker；
 * - AiExecutor 负责执行 AI 生成任务，避免阻塞 EventLoop。
 *
 * RpcServer 不负责：
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不直接依赖 FakeAiProvider；
 * - 不在 EventLoop 中执行慢速 AI 生成；
 * - 不让 worker 线程直接操作 TcpConnection。
 */
class RpcServer final {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;

    struct Options {
        AiExecutor::Options aiExecutorOptions{};
        RpcServerStreamResponder::Options streamResponderOptions{};
    };

    RpcServer(novanet::net::EventLoop* loop, const novanet::net::InetAddress& listenAddr,
              const std::string& name, AiProvider& aiProvider);

    RpcServer(novanet::net::EventLoop* loop, const novanet::net::InetAddress& listenAddr,
              const std::string& name, AiProvider& aiProvider, Options options);

    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    RpcServer(RpcServer&&) = delete;
    RpcServer& operator=(RpcServer&&) = delete;

    /*
     * 注册 protobuf service。
     *
     * 企业级约束：
     * - 建议 start() 前注册完成；
     * - 运行中不动态修改 registry，避免并发读写复杂化。
     */
    [[nodiscard]] bool registerService(google::protobuf::Service* service);

    /*
     * 设置 TcpServer I/O 线程数。
     * 必须在 start() 前调用。
     */
    void setThreadNum(int numThreads);

    /*
     * 启动 RpcServer。
     *
     * 启动顺序：
     * 1. AiExecutor::start()
     * 2. TcpServer::start()
     */
    [[nodiscard]] bool start();

    /*
     * 停止 RpcServer。
     *
     * 说明：
     * - 如果你的 TcpServer 还没有 stop()，这里不会强制停止 TcpServer；
     * - 但会 mark all responders closed；
     * - cancelAll streams；
     * - stop AiExecutor；
     * - 防止 worker 继续发送 late DATA。
     */
    void stop();

    [[nodiscard]] bool started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

private:
    struct ConnectionContext {
        explicit ConnectionContext(std::shared_ptr<StreamManager> manager)
            : streamManager(std::move(manager)) {
        }

        /*
         * 每个连接一个 RpcCodec。
         *
         * 虽然 RpcCodec 当前应尽量无状态，但连接级持有可以避免未来扩展状态时
         * 出现多个 sub loop 并发访问同一 codec 的问题。
         */
        RpcCodec codec;

        /*
         * 每个连接一个 StreamManager。
         *
         * stream_id 是连接内唯一，不是全服务器全局唯一。
         */
        std::shared_ptr<StreamManager> streamManager;

        /*
         * 每个连接一个 responder。
         *
         * AiExecutor worker 通过它异步发送 STREAM_DATA / STREAM_END。
         */
        std::shared_ptr<RpcServerStreamResponder> responder;
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

    void closeConnectionSafely(const TcpConnectionPtr& connection);

    void handleRpcMessage(const TcpConnectionPtr& connection,
                          const std::shared_ptr<ConnectionContext>& context,
                          const RpcMessage& message);

    void sendImmediateResponses(const TcpConnectionPtr& connection,
                                ConnectionContext& context,
                                std::vector<RpcMessage>& responses);

    void sendOneImmediateResponse(const TcpConnectionPtr& connection,
                                  ConnectionContext& context, const RpcMessage& response);

    [[nodiscard]] static ConnectionKey connectionKey(
        const TcpConnectionPtr& connection) noexcept;

private:
    novanet::net::EventLoop* loop_{nullptr};
    novanet::net::TcpServer server_;

    Options options_;

    ServiceRegistry registry_;
    MethodInvoker invoker_;

    /*
     * RpcServer 依赖 AiProvider 抽象。
     * FakeAiProvider / RealAiProvider 由外部注入。
     */
    AiProvider& aiProvider_;

    /*
     * AI 生成任务执行器。
     */
    AiExecutor aiExecutor_;

    /*
     * RpcDispatcher 不持有 StreamManager。
     * 每次 dispatch 时传入当前连接的 StreamManager。
     */
    RpcDispatcher dispatcher_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex contextsMutex_;
    std::unordered_map<ConnectionKey, std::shared_ptr<ConnectionContext>> contexts_;
};

}  // namespace novanet::rpc