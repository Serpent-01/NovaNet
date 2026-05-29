#pragma once

#include <google/protobuf/message.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "novanet/rpc/core/PendingCallManager.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::net {
class Buffer;
class TcpConnection;
}  // namespace novanet::net

namespace novanet::rpc {
class RpcChannel final {
public:
    explicit RpcChannel(std::shared_ptr<net::TcpConnection> conn);
    ~RpcChannel();

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;

    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    /*
     * 同步 unary RPC 调用入口。
     *
     * service:
     *   服务名，例如 "CalculatorService"。
     *
     * method:
     *   方法名，例如 "Add"。
     *
     * request:
     *   业务请求 protobuf，例如 AddRequest。
     *
     * response:
     *   业务响应 protobuf，例如 AddResponse。
     *
     * timeoutMs:
     *   等待响应的最大时间，单位毫秒。
     *
     * 返回 true:
     *   RPC 成功，response 已被填充。
     *
     * 返回 false:
     *   可能原因：
     *   - connection 为空
     *   - request 序列化失败
     *   - send 失败
     *   - 等待超时
     *   - 服务端返回错误
     *   - response 反序列化失败
     */
    [[nodiscard]] bool callUnary(const std::string& service,
                                 const std::string& method,
                                 const google::protobuf::Message& request,
                                 google::protobuf::Message& response,
                                 int timeoutMs);
    /*
     * TcpConnection 的 message callback 中调用。
     *
     * 负责从 Buffer 中解出 RpcMessage，
     * 然后根据 frame type 分发。
     */
    void onMessage(net::Buffer* buffer);

    /*
     * 连接关闭时调用。
     *
     * 必须唤醒所有正在等待的 callUnary，
     * 否则调用线程可能永久阻塞。
     */
    void onConnectionClosed();

    [[nodiscard]] std::uint32_t openStream(
        const std::string& service, const std::string& method,
        const google::protobuf::Message& request);
    [[nodiscard]] bool sendStreamData(std::uint32_t streamId,
                                      const std::string& chunk);

    [[nodiscard]] bool sendStreamEnd(std::uint32_t streamId);

    [[nodiscard]] bool cancelStream(std::uint32_t streamId);

private:
    [[nodiscard]] std::uint64_t nextRequestId() noexcept;
    [[nodiscard]] std::uint32_t nextStreamId() noexcept;

    [[nodiscard]] bool buildUnaryRequestMessage(
        const std::string& service, const std::string& method,
        const google::protobuf::Message& request, std::uint64_t requestId,
        RpcMessage* outMessage) const;

    [[nodiscard]] bool sendMessage(const RpcMessage& message);

    void handleUnaryResponse(const RpcMessage& message);
    void handleErrorFrame(const RpcMessage& message);
    void handleDecodeError();

private:
    std::shared_ptr<net::TcpConnection> connection_;
    RpcCodec codec_;
    PendingCallManager pendingCalls_;
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::atomic<std::uint32_t> nextStreamId_{1};
};
}  // namespace novanet::rpc