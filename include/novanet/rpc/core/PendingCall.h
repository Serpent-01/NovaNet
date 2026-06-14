#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace novanet::rpc {
/*
 * PendingCall 表示客户端侧一个正在等待响应的 unary RPC 请求。
 *
 * 一个 PendingCall 只对应一个 request_id。
 *
 * 典型流程：
 *
 * 1. RpcChannel::callUnary()
 *      创建 PendingCall
 *      发送 UNARY_REQUEST
 *      调用 waitFor() 等待响应
 *
 * 2. RpcChannel::onMessage()
 *      收到 UNARY_RESPONSE
 *      根据 request_id 找到 PendingCall
 *      调用 markDone()
 *
 * 3. 如果等待超时
 *      waitFor() 内部会把状态变成 kTimeout
 *
 * 线程模型：
 * - 调用线程可能在 waitFor() 中等待。
 * - EventLoop 线程可能收到响应后调用 markDone()。
 * - 连接关闭 / 管理器清理时可能调用 markFailed()。
 *
 * 所以本类内部所有可变状态都由 mutex_ 保护。
 */
class PendingCall final {
public:
    enum class State : std::uint8_t {
        kPending = 0,  // 还在等待响应
        kDone,         // 已收到正常响应
        kTimeout,      // 等待超时
        kFailed,       // 发送失败，连接关闭，服务端错误等
    };

    explicit PendingCall(std::uint64_t request_id) noexcept;

    ~PendingCall() = default;

    PendingCall(const PendingCall&) = delete;
    PendingCall& operator=(const PendingCall&) = delete;

    PendingCall(PendingCall&&) = delete;
    PendingCall& operator=(const PendingCall&&) = delete;

    [[nodiscard]] std::uint64_t requestId() const noexcept;

    [[nodiscard]] State state() const;
    [[nodiscard]] bool pending() const;
    [[nodiscard]] bool done() const;
    [[nodiscard]] bool timeout() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] bool completed() const;

    /*
     * 返回拷贝，不返回引用。
     *
     * 原因：
     * responseBytes_ / errorText_ 受 mutex_ 保护。
     * 如果返回 const std::string&，调用方可能在锁释放后继续访问内部对象，
     * 容易破坏线程安全边界。
     */
    [[nodiscard]] std::string responseBytes() const;
    [[nodiscard]] std::string errorText() const;

    /*
     * markDone:
     *   正常收到服务端 UNARY_RESPONSE。
     *
     * 返回 true:
     *   本次调用成功把状态从 kPending 改成 kDone。
     *
     * 返回 false:
     *   说明这个 call 已经 timeout / failed / done。
     *   这可以避免重复完成、迟到响应覆盖超时状态。
     */
    [[nodiscard]] bool markDone(std::string responseBytes);

    /*
     * markTimeout:
     *   把请求标记为超时。
     *
     * 通常由 waitFor() 内部调用。
     * PendingCallManager 也可以调用它做集中超时管理。
     */
    [[nodiscard]] bool markTimeout(std::string errorText = "rpc call timeout");

    /*
     * markFailed:
     *   把请求标记为失败。
     *
     * 常见原因：
     * - 发送失败
     * - 连接关闭
     * - 服务端返回 ERROR_FRAME
     * - 解析响应失败
     */
    [[nodiscard]] bool markFailed(std::string errorText);

    /*
     * wait:
     *   一直阻塞，直到进入终态。
     *
     * waitFor:
     *   最多等待 duration。
     *   如果超时前没有收到响应，会把状态改成 kTimeout。
     */
    [[nodiscard]] State wait();
    [[nodiscard]] State waitFor(std::chrono::milliseconds duration);

    [[nodiscard]] static std::string_view stateToString(State state) noexcept;

private:
    [[nodiscard]] static bool isTerminal(State state) noexcept;

private:
    const std::uint64_t requestId_{0};

    mutable std::mutex mutex_;
    std::condition_variable cond_;

    State state_{State::kPending};
    std::string responseBytes_;
    std::string errorText_;
};
}  // namespace novanet::rpc