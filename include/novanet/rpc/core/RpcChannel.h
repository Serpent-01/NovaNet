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

class RpcChannel final {
public:
    using TcpConnectionPtr = novanet::net::TcpConnection::TcpConnectionPtr;

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

        std::function<void(std::uint32_t, novanet::rpc::meta::RpcErrorCode, std::string)>
            onEnd;

        std::function<void(std::uint32_t, novanet::rpc::meta::RpcErrorCode, std::string)>
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

    [[nodiscard]] RpcStatus callUnary(const std::string& serviceName,
                                      const std::string& methodName,
                                      const google::protobuf::Message& request,
                                      google::protobuf::Message* response,
                                      std::chrono::milliseconds timeout);

    [[nodiscard]] StreamHandle openStream(const std::string& serviceName,
                                          const std::string& methodName,
                                          const google::protobuf::Message& request,
                                          StreamCallbacks callbacks);

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
                                             const std::string& requestPayload);

    [[nodiscard]] bool sendStreamCancelMessage(std::uint32_t streamId,
                                               std::uint64_t requestId,
                                               std::string reason);

    [[nodiscard]] bool sendHeartbeatPong(const RpcMessage& ping);

    void handleRpcMessage(const RpcMessage& message);
    void handleUnaryResponse(const RpcMessage& message);
    void handleStreamData(const RpcMessage& message);
    void handleStreamEnd(const RpcMessage& message);
    void handleStreamCancel(const RpcMessage& message);
    void handleErrorFrame(const RpcMessage& message);
    void handleHeartbeatPing(const RpcMessage& message);
    void handleHeartbeatPong(const RpcMessage& message);

    void saveCallbacks(std::uint32_t streamId, StreamCallbacks callbacks);
    void eraseCallbacks(std::uint32_t streamId);
    std::optional<StreamCallbacks> takeCallbacks(std::uint32_t streamId);
    std::optional<StreamCallbacks> findCallbacks(std::uint32_t streamId) const;

    void failStream(std::uint32_t streamId, novanet::rpc::meta::RpcErrorCode errorCode,
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

    novanet::net::TimerId heartbeatPingTimer_;
    novanet::net::TimerId heartbeatCheckTimer_;
    novanet::net::TimerId streamTimeoutTimer_;
};

}  // namespace novanet::rpc
