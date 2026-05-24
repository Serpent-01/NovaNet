#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "novanet/net/Buffer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/RpcDispatcher.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace google {
namespace protobuf {
class Service;
}
}  // namespace google

namespace novanet::rpc {
class RpcServer final {
public:
    RpcServer(novanet::net::EventLoop* loop,
              const novanet::net::InetAddress& listenAddr, std::string name);
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    RpcServer(RpcServer&&) = delete;
    RpcServer& operator=(RpcServer&&) = delete;

    ~RpcServer() = default;

    //注册 protobuf service。
    [[nodiscard]] bool registerService(google::protobuf::Service* service,
                                       std::string* errorText = nullptr);
    /*
     * 启动底层 TcpServer。
     * - 先 registerService()
     * - 再 start()
     */
    void start();
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] std::size_t serviceCount() const noexcept;

    [[nodiscard]] ServiceRegistry& registry() noexcept;
    [[nodiscard]] const ServiceRegistry& registry() const noexcept;

private:
    void installCallbacks();
    void handleMessage(const std::shared_ptr<novanet::net::TcpConnection>& conn,
                       novanet::net::Buffer* buffer);
    bool sendRpcMessage(
        const std::shared_ptr<novanet::net::TcpConnection>& conn,
        const RpcMessage& msg) const;
    void closeConnection(
        const std::shared_ptr<novanet::net::TcpConnection>& conn) const;

private:
    novanet::net::TcpServer tcpServer_;
    RpcCodec codec_;
    ServiceRegistry registry_;
    MethodInvoker invoker_;
    RpcDispatcher dispatcher_;

    bool started_{false};
};
}  // namespace novanet::rpc