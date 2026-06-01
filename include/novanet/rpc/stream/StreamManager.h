#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novanet/base/Timestamp.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamSession.h"

namespace novanet::rpc {
/*
 * StreamManager 管理一个连接上的所有逻辑 stream。
 *
 * 核心职责：
 * - stream_id -> StreamSession 映射；
 * - 创建 / 查找 / 删除 stream；
 * - 路由 STREAM_DATA / STREAM_END / STREAM_CANCEL；
 * - cancelAll；
 * - timeoutStreams；
 * - 保证多个 stream 不串。
 */
class StreamManager final {
public:
    using SessionPtr = std::shared_ptr<StreamSession>;
    using DataCallback = StreamSession::DataCallback;
    using EndCallback = StreamSession::EndCallback;
    using ErrorCallback = StreamSession::ErrorCallback;

    enum class Result : std::uint8_t {
        kOk = 0,
        kInvalidArgument,
        kInvalidFrame,
        kDuplicateStream,
        kStreamNotFound,
        kStateRejected,
    };

    StreamManager() = default;
    ~StreamManager() = default;

    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    StreamManager(StreamManager&&) = delete;
    StreamManager& operator=(StreamManager&&) = delete;

    /*
     * 创建一个新的 stream。
     *
     * 返回 nullptr 表示：
     * - streamId == 0；
     * - requestId == 0；
     * - serviceName / methodName 为空；
     * - streamId 已存在。
     */
    [[nodiscard]] SessionPtr
    createStream(std::uint32_t streamId, std::uint64_t requestId,
                 std::string serviceName, std::string methodName,
                 DataCallback onData = nullptr, EndCallback onEnd = nullptr,
                 ErrorCallback onError = nullptr);
    /*
     * 注册外部创建好的 StreamSession。
     *
     * 主要用于测试或特殊构造场景。
     * 正常业务优先使用 createStream()。
     */
    [[nodiscard]] bool addStream(SessionPtr session);

    /*
     * 查找 stream。
     *
     * 返回 shared_ptr 是为了保证：
     * 调用方临时使用期间，即使 manager 中被 remove，
     * session 对象也不会立即析构。
     */
    [[nodiscard]] SessionPtr findStream(std::uint32_t streamId) const;

    /*
     * 移除 stream。
     *
     * 只移除映射，不修改 StreamSession 状态。
     * 状态变化由 handleEndFrame / handleCancelFrame / timeoutStreams 负责。
     */
    [[nodiscard]] SessionPtr removeStream(std::uint32_t streamId);

    /*
     * 本端发送 STREAM_END 后调用。
     *
     * 例如 RpcChannel::sendStreamEnd() 成功发送后，
     * 应调用 markLocalEnd(streamId)。
     */
    [[nodiscard]] Result markLocalEnd(std::uint32_t streamId);

    /*
     * 收到 STREAM_DATA 后调用。
     *
     * 要求：
     * - msg 必须是合法 STREAM_DATA；
     * - stream_id 必须存在；
     * - session 当前必须允许接收 DATA。
     */
    [[nodiscard]] Result handleDataFrame(const RpcMessage& msg);

    /*
     * 收到 STREAM_END 后调用。
     *
     * 行为：
     * - 路由到对应 StreamSession；
     * - 调用 session->notifyEnd()；
     * - 如果 session 进入 terminal 状态，则从 map 移除。
     */
    [[nodiscard]] Result handleEndFrame(const RpcMessage& msg);

    /*
     * 收到 STREAM_CANCEL 后调用。
     *
     * 行为：
     * - 只取消对应 stream_id；
     * - 从 map 移除；
     * - 调用 session->markCancelled(reason)。
     */
    [[nodiscard]] Result handleCancelFrame(const RpcMessage& msg);

    /*
     * 连接关闭 / RpcChannel 析构 / RpcServer 关闭连接时调用。
     *
     * 会移除所有 stream，并在不持有 manager 锁的情况下逐个 markCancelled。
     */
    [[nodiscard]] std::size_t
    cancelAll(std::string reason = "connection closed");

    /*
     * 扫描超时 stream。
     *
     * 注意：
     * StreamManager 不直接持有 TimerQueue。
     * 上层 RpcServer / RpcChannel 使用 EventLoop/TimerQueue 周期性调用本函数。
     *
     * 返回被 timeout 清理的 stream_id 列表。
     */
    [[nodiscard]] std::vector<std::uint32_t>
    timeoutStreams(novanet::base::Timestamp now, double timeoutSeconds,
                   std::string reason = "stream idle timeout");

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

    [[nodiscard]] std::vector<std::uint32_t> activeStreamIds() const;

    [[nodiscard]] static std::string_view
    resultToString(Result result) noexcept;

private:
    [[nodiscard]] static bool validStreamId(std::uint32_t streamId) noexcept;
    [[nodiscard]] static bool validRequestId(std::uint64_t requestId) noexcept;

    [[nodiscard]] bool removeStreamIfSame(std::uint32_t streamId,
                                          const SessionPtr& expected);

    [[nodiscard]] std::vector<SessionPtr> snapshotSessions() const;

private:
    mutable std::mutex mutex_;

    std::unordered_map<std::uint32_t, SessionPtr> streams_;
};

} // namespace novanet::rpc