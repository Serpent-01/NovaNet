#pragma once

#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include <string>
#include <vector>

namespace novanet::rpc {
/*
 * RpcController 表示一次 RPC 调用的状态。
 *
 * 第一版职责：
 * 1. 记录调用是否失败
 * 2. 记录调用是否超时
 * 3. 保存错误文本
 * 4. 兼容 protobuf generic service 的 google::protobuf::RpcController 接口
 *
 * 注意：
 * - RpcController 不负责网络 I/O。
 * - RpcController 不负责 request_id 映射。
 * - RpcController 不负责等待 response。
 * - 每次 RPC 调用应该使用一个独立的 RpcController。
 *
 * 第一版不设计成线程安全对象。
 * 它通常在 MethodInvoker 同步调用 service method 的过程中使用。
 */
class RpcController final : public google::protobuf::RpcController {
public:
    RpcController() = default;
    ~RpcController() override = default;

    RpcController(const RpcController&) = delete;
    RpcController& operator=(const RpcController&) = delete;

    RpcController(RpcController&&) = delete;
    RpcController& operator=(RpcController&&) = delete;

public:
    // =================================================================
    // Protobuf 官方标准接口 (供框架底层多态回调或业务代码使用)
    // =================================================================

    /// @brief 清除所有状态，重置为初始化状态。
    void Reset() override;

    /// @brief 检查本次 RPC 调用是否出错失败
    [[nodiscard]] bool Failed() const override;

    /// @brief 获取调用失败的具体文字描述
    [[nodiscard]] std::string ErrorText() const override;

    /// @brief 主动触发取消当前RPC调用 (如：客户端断网)
    void StartCancel() override;

    /// @brief 将当前调用标记为失败，并填入原因
    void SetFailed(const std::string& reason) override;

    /// @brief 将当前调用标记为超时
    void SetTimeout(const std::string& reason = "rpc timeout");

    /// @brief 将当前调用标记为取消
    void SetCancelled(const std::string& reason = "rpc cancelled");

    /// @brief 检查当前调用是否已被取消
    [[nodiscard]] bool IsCanceled() const override;

    /// @brief 注册一个取消回调函数，当 StartCancel() 被调用时触发该回调
    void NotifyOnCancel(google::protobuf::Closure* callback) override;

public:
    /*
     * NovaNet 自己更符合项目风格的小写接口。
     *
     * 后续 MethodInvoker / RpcDispatcher 可以用这些接口，
     * 业务 service 里也可以通过 google::protobuf::RpcController 接口使用。
     */
    // =================================================================
    // NovaNet 原生接口 (无虚函数开销，推荐框架内部流程使用)
    // =================================================================

    /// @brief 内部极速重置状态
    void reset();

    /// @brief 无异常抛出的失败状态查询
    [[nodiscard]] bool failed() const noexcept;

    /// @brief 检查是否发生网络 或业务超时
    [[nodiscard]] bool timeout() const noexcept;

    /// @brief 检查是否处于取消状态
    [[nodiscard]] bool canceled() const noexcept;

    /// @brief 获取错误信息的常引用（避免 std::string 拷贝开销）
    [[nodiscard]] const std::string& errorText() const noexcept;

    /// @brief 设置失败原因
    void setFailed(std::string reason);

    /*
     * 设置 timeout 状态。
     * 开/关超时状态（开启时自动设置 failed_ 和默认提示）
     * on == true:
     *   timeout_ = true
     *   failed_ = true
     *   errorText_ 默认为 "rpc timeout"
     *
     * on == false:
     *   只清理 timeout_。
     *   如果当前错误就是默认 timeout 错误，则同时清理 failed_ 和 errorText_。
     */
    void setTimeout(bool on);

    /// @brief 开/关超时状态，并附带自定义的超时提示原因
    void setTimeout(bool on, std::string reason);

    /// @brief 设置取消状态，并触发已注册取消回调
    void setCancelled(std::string reason = "rpc cancelled");

private:
    /// @brief 内部辅助函数：执行所有已注册的取消回调
    void runCancelCallbacks();

private:
    /// @brief 失败标志位
    bool failed_{false};

    /// @brief 超时标志位
    bool timeout_{false};

    /// @brief 取消标志位
    bool canceled_{false};

    /// @brief 详细的错误文本信息
    std::string errorText_;

    /// @brief 存储取消事件触发时需要执行的闭包回调
    std::vector<google::protobuf::Closure*> cancelCallbacks_;
};

}  // namespace novanet::rpc
