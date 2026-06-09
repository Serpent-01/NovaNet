#pragma once

#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "chat.pb.h"
#include "novanet/base/Timestamp.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/TcpConnection.h"
#include "novanet/net/TimerId.h"
#include "novanet/rpc/core/PendingCallManager.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * RpcChannel 是客户端侧 RPC 协议收发通道。
 *
 * 职责：
 * - 发送 UNARY_REQUEST；
 * - 接收 UNARY_RESPONSE；
 * - 发送 STREAM_OPEN；
 * - 接收 STREAM_DATA / STREAM_END；
 * - 发送 STREAM_CANCEL；
 * - 处理 HEARTBEAT_PING / HEARTBEAT_PONG；
 * - 管理 request_id / stream_id；
 * - 管理 PendingCall / StreamManager；
 * - 支持 metadata 透传到 UnaryRequestMeta / StreamOpenMeta。
 *
 * 不负责：
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试；
 * - 不直接管理 TcpClient 生命周期。
 */
class RpcChannel final : public std::enable_shared_from_this<RpcChannel> {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;
    using MetadataMap = std::unordered_map<std::string, std::string>;

    struct Options {
        std::size_t sendHighWaterMarkBytes{8 * 1024 * 1024};

        double heartbeatIntervalSeconds{10.0};
        double heartbeatCheckIntervalSeconds{5.0};
        double heartbeatTimeoutSeconds{30.0};

        double streamTimeoutScanIntervalSeconds{5.0};
        double streamIdleTimeoutSeconds{60.0};

        std::string nodeId{"novanet-client"};
    };

    struct StreamCallbacks {
        std::function<void(std::uint32_t, std::uint64_t,
                           const novanet::ai::chat::GenerateChunk&)>
            onData;

        std::function<void(std::uint32_t, novanet::rpc::meta::RpcErrorCode,
                           std::string)>
            onEnd;

        std::function<void(std::uint32_t, novanet::rpc::meta::RpcErrorCode,
                           std::string)>
            onError;
    };

    struct StreamHandle {
        std::uint32_t streamId{0};
        std::uint64_t requestId{0};
        bool ok{false};
        std::string errorText;

        [[nodiscard]] explicit operator bool() const noexcept {
            return ok;
        }
    };

    explicit RpcChannel(TcpConnectionPtr connection);
    RpcChannel(TcpConnectionPtr connection, Options options);
    ~RpcChannel();

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;

    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    /*
     * 旧接口：无 metadata。
     * 内部转发到 metadata 版本。
     */
    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout);

    /*
     * 新接口：支持 metadata。
     * metadata 会写入 UnaryRequestMeta.metadata。
     */
    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout,
                                      const MetadataMap& metadata);

    /*
     * 新接口：支持 metadata + requestId 回填。
     *
     * requestIdOut:
     * - 可以为 nullptr；
     * - 非 nullptr 时，在生成 request_id 后写回，方便 SDK ClientContext 记录。
     */
    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout,
                                      const MetadataMap& metadata,
                                      std::uint64_t* requestIdOut);

    /*
     * 旧接口：无 metadata。
     * 内部转发到 metadata 版本。
     */
    [[nodiscard]] StreamHandle openStream(const std::string& serviceName,
                                          const std::string& methodName,
                                          const google::protobuf::Message& request,
                                          StreamCallbacks callbacks);

    /*
     * 新接口：支持 metadata。
     * metadata 会写入 StreamOpenMeta.metadata。
     */
    [[nodiscard]] StreamHandle openStream(const std::string& serviceName,
                                          const std::string& methodName,
                                          const google::protobuf::Message& request,
                                          StreamCallbacks callbacks,
                                          const MetadataMap& metadata);

    [[nodiscard]] bool cancelStream(std::uint32_t streamId,
                                    std::string reason = "client cancelled");

    void onMessage(const TcpConnectionPtr& connection, novanet::net::Buffer* buffer);

    void onConnectionClosed(std::string reason);

    void startTimers();
    void stopTimers();

    [[nodiscard]] bool sendHeartbeatPing();
    [[nodiscard]] bool checkHeartbeatTimeout();
    void checkStreamTimeouts();

private:
    [[nodiscard]] std::uint64_t nextRequestId();
    [[nodiscard]] std::uint32_t nextStreamId();

    [[nodiscard]] bool sendRpcMessage(RpcMessage message);

    [[nodiscard]] bool sendStreamOpenMessage(std::uint32_t streamId,
                                             std::uint64_t requestId,
                                             const std::string& serviceName,
                                             const std::string& methodName,
                                             const std::string& requestPayload,
                                             const MetadataMap& metadata);

    [[nodiscard]] bool sendStreamCancelMessage(std::uint32_t streamId,
                                               std::uint64_t requestId,
                                               std::string reason);

    [[nodiscard]] bool sendHeartbeatPong(const RpcMessage& ping);

private:
    void handleRpcMessage(const RpcMessage& message);
    void handleUnaryResponse(const RpcMessage& message);
    void handleStreamData(const RpcMessage& message);
    void handleStreamEnd(const RpcMessage& message);
    void handleStreamCancel(const RpcMessage& message);
    void handleErrorFrame(const RpcMessage& message);
    void handleHeartbeatPing(const RpcMessage& message);
    void handleHeartbeatPong(const RpcMessage& message);

private:
    void saveCallbacks(std::uint32_t streamId, StreamCallbacks callbacks);
    void eraseCallbacks(std::uint32_t streamId);

    [[nodiscard]] std::optional<StreamCallbacks> takeCallbacks(
        std::uint32_t streamId);

    [[nodiscard]] std::optional<StreamCallbacks> findCallbacks(
        std::uint32_t streamId) const;

    void failStream(std::uint32_t streamId,
                    novanet::rpc::meta::RpcErrorCode errorCode,
                    std::string errorText);

    [[nodiscard]] static StreamHandle makeStreamError(std::string errorText);

private:
    TcpConnectionPtr connection_;
    Options options_;

    RpcCodec codec_;
    PendingCallManager pendingCalls_;
    StreamManager streamManager_;

    std::atomic<std::uint64_t> nextRequestId_{1};
    std::atomic<std::uint32_t> nextStreamId_{1};
    std::atomic<bool> connectionClosed_{false};

    mutable std::mutex callbacksMutex_;
    std::unordered_map<std::uint32_t, StreamCallbacks> callbacks_;

    std::atomic<std::int64_t> lastPingMicros_{0};
    std::atomic<std::int64_t> lastPongMicros_{0};

    std::mutex timersMutex_;
    bool timersStarted_{false};
    novanet::net::TimerId heartbeatPingTimer_;
    novanet::net::TimerId heartbeatCheckTimer_;
    novanet::net::TimerId streamTimeoutTimer_;
};

}  // namespace novanet::rpc