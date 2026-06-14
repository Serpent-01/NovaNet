#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "novanet/rpc/core/PendingCall.h"

namespace novanet::rpc {
/*
 * PendingCallManager 管理客户端侧所有未完成的 unary RPC 请求。
 *
 * 核心职责：
 * - request_id -> PendingCall 映射；
 * - response 到达时按 request_id 找到正确 PendingCall；
 * - timeout / failed 时清理对应 PendingCall；
 * - 连接关闭时 failAll() 唤醒所有等待者；
 * - 防止 response 串请求、防止重复完成、防止超时后迟到响应覆盖状态。
 *
 * 它不负责：
 * - 不发送 socket；
 * - 不解析 protobuf；
 * - 不处理 RpcCodec；
 * - 不分发 RpcDispatcher；
 * - 不管理 stream_id。
 *
 * 这些职责分别属于 RpcChannel / RpcCodec / RpcDispatcher / StreamManager。
 */
class PendingCallManager final {
public:
    enum class FinishResult : std::uint8_t {
        kCompleted = 0,  // 本次成功完成
        kNotFound,  // request_id 不存在，通常是迟到响应或未知响应
        kAlreadyFinished,  // call 已经 timeout / failed / done
    };
    PendingCallManager() = default;
    ~PendingCallManager();

    PendingCallManager(const PendingCallManager&) = delete;
    PendingCallManager& operator=(const PendingCallManager&) = delete;

    PendingCallManager(PendingCallManager&&) = delete;
    PendingCallManager& operator=(PendingCallManager&&) = delete;

    /*
     * 创建并注册一个 PendingCall。
     *
     * 返回 nullptr 表示：
     * - requestId == 0；
     * - requestId 已经存在。
     */
    [[nodiscard]] std::shared_ptr<PendingCall> create(std::uint64_t requestId);

    /*
     * 注册外部创建的 PendingCall。
     *
     * 一般业务代码推荐使用 create()；
     * add() 主要用于测试或更灵活的构造场景。
     */
    [[nodiscard]] bool add(std::uint64_t requsetId,
                           std::shared_ptr<PendingCall> call);

    /*
     * 查找 PendingCall。
     *
     * 返回 shared_ptr 是为了保证：
     * 调用方临时使用期间，即使 manager 中被 remove，
     * 对象也不会立即析构。
     */
    [[nodiscard]] std::shared_ptr<PendingCall> find(
        std::uint64_t requestId) const;

    /*
     * 从 map 中移除 PendingCall。
     *
     * 注意：
     * remove() 只移除映射，不改变 PendingCall 状态。
     * 状态改变由 complete / timeout / fail 或 PendingCall 自己负责。
     */
    [[nodiscard]] std::shared_ptr<PendingCall> remove(std::uint64_t requestId);

    /*
     * 收到正常 UNARY_RESPONSE 时调用。
     *
     * 语义：
     * - 找到 request_id 对应的 PendingCall；
     * - 先从 map 移除，避免后续重复 response 再命中；
     * - 调用 call->markDone(responseBytes) 唤醒等待线程。
     */
    [[nodiscard]] FinishResult complete(std::uint64_t requestId,
                                        std::string responseBytes);

    /*
     * 把某个请求标记为失败。
     *
     * 常见原因：
     * - 服务端返回 ERROR_FRAME；
     * - 解析响应失败；
     * - 发送失败；
     * - 连接关闭。
     */
    [[nodiscard]] FinishResult fail(std::uint64_t requestId,
                                    std::string errorText);

    /*
     * 把某个请求标记为超时。
     *
     * 如果你当前 RpcChannel::callUnary() 使用 PendingCall::waitFor()，
     * 那通常是 waitFor() 自己把状态变成 timeout，
     * 然后 callUnary() 再调用 remove() 清理 map。
     *
     * 这个 timeout() 接口保留给后续集中式 TimerQueue 超时管理。
     */
    [[nodiscard]] FinishResult timeout(
        std::uint64_t requestId, std::string errorText = "rpc call timeout");

    /*
     * 连接关闭 / codec error / RpcChannel 析构时调用。
     *
     * 它会把当前所有 pending calls 移出 map，
     * 然后在不持有 manager 锁的情况下逐个 markFailed()。
     */
    [[nodiscard]] std::size_t failAll(std::string errorText);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

    /*
     * 测试/调试使用。
     */
    [[nodiscard]] std::vector<std::uint64_t> activeRequestIds() const;

private:
    [[nodiscard]] static bool validRequestId(std::uint64_t requestId) noexcept;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;
};
}  // namespace novanet::rpc