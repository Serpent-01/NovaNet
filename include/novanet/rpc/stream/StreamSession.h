#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "novanet/base/Timestamp.h"

namespace novanet::rpc {
/*
 * StreamSession 表示一个 stream_id 对应的逻辑流状态。
 *
 * 它负责：
 * - 保存 streamId / requestId；
 * - 保存 serviceName / methodName；
 * - 管理 open / half-closed / closed / cancelled 状态；
 * - 记录 lastActiveTime，用于 stream timeout；
 * - 判断当前是否还能发送/接收 STREAM_DATA；
 * - 触发 onData / onEnd / onError 回调。
 */
class StreamSession final {
public:
    /* kOpen:
    双方都还没发送 STREAM_END。
    本端可以发 DATA，也可以收 DATA。

    kHalfClosedLocal:
    本端已经发送 STREAM_END。
    本端不能再发 DATA。
    但对端还没 END，所以本端仍然可以收 DATA。

    kHalfClosedRemote:
    对端已经发送 STREAM_END。
    本端不能再收 DATA。
    但本端还没 END，所以本端仍然可以发 DATA。

    kClosed:
    双方都已经 END。
    不能再发 DATA，也不能再收 DATA。

    kCancelled:
    流被取消。
    不能再发 DATA，也不能再收 DATA。 */
    enum class State : std::uint8_t {
        kOpen = 0,
        kHalfClosedLocal,
        kHalfClosedRemote,
        kClosed,
        kCancelled,
    };

    using DataCallback =
        std::function<void(std::uint32_t streamId, const std::string& payload)>;

    using EndCallback = std::function<void(std::uint32_t streamId)>;

    using ErrorCallback = std::function<void(std::uint32_t streamId,
                                             const std::string& errorText)>;

    StreamSession(std::uint32_t streamId, std::uint64_t requestId,
                  std::string serviceName, std::string methodName);

    ~StreamSession() = default;

    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    StreamSession(StreamSession&&) = delete;
    StreamSession& operator=(StreamSession&&) = delete;

    [[nodiscard]] std::uint32_t streamId() const noexcept;
    [[nodiscard]] std::uint64_t requestId() const noexcept;

    [[nodiscard]] std::string serviceName() const;
    [[nodiscard]] std::string methodName() const;

    [[nodiscard]] State state() const;
    [[nodiscard]] std::string cancelReason() const;

    [[nodiscard]] bool open() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] bool terminal() const;

    [[nodiscard]] bool localEndSent() const;
    [[nodiscard]] bool remoteEndReceived() const;

    /*
     * 当前本端是否还能发送 STREAM_DATA。
     *
     * kOpen:
     *   双方都没结束，可以发送。
     *
     * kHalfClosedRemote:
     *   对端已经 END，但本端还没 END。
     *   对 full-duplex 语义来说，本端仍然可以发送。
     *
     * kHalfClosedLocal / kClosed / kCancelled:
     *   本端已经 END 或流已终止，不能再发 DATA。
     */
    [[nodiscard]] bool canSendData() const;

    /*
     * 当前本端是否还能接收 STREAM_DATA。
     *
     * kOpen:
     *   双方都没结束，可以接收。
     *
     * kHalfClosedLocal:
     *   本端已经 END，但对端还没 END。
     *   对 full-duplex 语义来说，仍然可以接收。
     *
     * kHalfClosedRemote / kClosed / kCancelled:
     *   对端已经 END 或流已终止，不能再接收 DATA。
     */
    [[nodiscard]] bool canReceiveData() const;

    /*
     * 本端发送 STREAM_END 后调用。
     *
     * kOpen:
     *   -> kHalfClosedLocal
     *
     * kHalfClosedRemote:
     *   -> kClosed
     *
     * 其他状态：
     *   返回 false
     */
    [[nodiscard]] bool markLocalEnd();

    /*
     * 只更新“收到对端 STREAM_END”的状态。
     *
     * 注意：
     * - markRemoteEnd() 只做状态转换，不触发 onEnd callback。
     * - 如果你希望状态转换 + callback，一般调用 notifyEnd()。
     */
    [[nodiscard]] bool markRemoteEnd();

    /*
     * 收到/发送 STREAM_CANCEL 后调用。
     *
     * 任何非终态都可以进入 kCancelled。
     * 进入 cancelled 后不能再发送/接收 DATA。
     *
     * 会触发 onError callback。
     */
    [[nodiscard]] bool markCancelled(std::string reason);

    /*
     * 超时关闭。
     *
     * 语义上等价于 cancelled，但 errorText 更明确。
     */
    [[nodiscard]] bool markTimeout(std::string reason = "stream timeout");

    /*
     * 活跃时间。
     *
     * 用于 StreamManager::timeoutStreams()。
     */
    void touch();
    void touch(novanet::base::Timestamp now);

    [[nodiscard]] novanet::base::Timestamp lastActiveTime() const;

    /*
     * 返回当前距离上次活跃过去了多少秒。
     */
    [[nodiscard]] double idleSeconds(novanet::base::Timestamp now) const;

    /*
     * 判断是否超过 stream idle timeout。
     */
    [[nodiscard]] bool expired(novanet::base::Timestamp now,
                               double timeoutSeconds) const;

    void setDataCallback(DataCallback cb);
    void setEndCallback(EndCallback cb);
    void setErrorCallback(ErrorCallback cb);

    /*
     * 收到 STREAM_DATA 后调用。
     *
     * 行为：
     * - 如果当前不能接收 DATA，返回 false；
     * - 更新 lastActiveTime；
     * - 解锁后触发 onData。
     *
     * 注意：
     * notifyData 不解析 payload。
     * payload 是 GenerateChunk 还是其他业务 protobuf bytes，由上层决定。
     */
    [[nodiscard]] bool notifyData(const std::string& payload);

    /*
     * 收到对端 STREAM_END 后调用。
     *
     * 行为：
     * - 内部完成 remote end 状态转换；
     * - 更新 lastActiveTime；
     * - 解锁后触发 onEnd。
     */
    [[nodiscard]] bool notifyEnd();

    /*
     * 只触发 error callback，不改变状态。
     *
     * 一般更推荐使用 markCancelled()/markTimeout()。
     * 这个函数用于上层已经完成状态转换，只想通知错误的场景。
     */
    void notifyError(const std::string& errorText);

    [[nodiscard]] static bool canSendInState(State state) noexcept;
    [[nodiscard]] static bool canReceiveInState(State state) noexcept;
    [[nodiscard]] static bool isTerminal(State state) noexcept;
    [[nodiscard]] static std::string_view stateToString(State state) noexcept;

private:
    const std::uint32_t streamId_{0};
    const std::uint64_t requestId_{0};

    mutable std::mutex mutex_;

    State state_{State::kOpen};

    std::string serviceName_;
    std::string methodName_;

    bool localEndSent_{false};
    bool remoteEndReceived_{false};

    std::string cancelReason_;

    novanet::base::Timestamp lastActiveTime_{
        novanet::base::Timestamp::invalid()};

    DataCallback onData_;
    EndCallback onEnd_;
    ErrorCallback onError_;
};
}  // namespace novanet::rpc