#pragma once

#include <memory>
#include <utility>

#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ClientChannel.h"

namespace novanet::rpc::sdk {

/*
 * StubBase 是所有 SDK 业务 Stub 的公共基类。
 *
 * 典型派生类：
 * - CalculatorServiceStub
 * - ChatServiceStub
 *
 * 职责：
 * - 保存 std::shared_ptr<ClientChannel>；
 * - 提供 channel() 给派生 Stub 使用；
 * - 提供 valid() / connected() / ensureChannelReady()；
 * - 不直接依赖 RpcClient；
 * - 不直接依赖 RpcChannel；
 * - 不直接处理 TcpConnection；
 * - 不处理 protobuf 编解码细节。
 *
 * 设计边界：
 * - StubBase 不知道具体 service_name / method_name；
 * - StubBase 不知道 Add / Generate；
 * - StubBase 不做服务发现；
 * - StubBase 不做负载均衡；
 * - StubBase 不做重试；
 * - StubBase 不做认证；
 * - StubBase 不做拦截器。
 */
class StubBase {
public:
    StubBase() = delete;

    explicit StubBase(std::shared_ptr<ClientChannel> channel)
        : channel_(std::move(channel)) {
    }

    virtual ~StubBase() = default;

    /*
     * Stub 本身是轻量对象，只持有 shared_ptr<ClientChannel>。
     *
     * 允许拷贝：
     * - 复制 Stub 不会复制底层连接；
     * - 只是共享同一个 ClientChannel；
     * - 对 SDK 使用更方便。
     *
     * 如果你想更保守，也可以把 copy delete。
     */
    StubBase(const StubBase&) = default;
    StubBase& operator=(const StubBase&) = default;

    StubBase(StubBase&&) noexcept = default;
    StubBase& operator=(StubBase&&) noexcept = default;

    /*
     * 当前 Stub 是否持有有效 channel。
     */
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(channel_);
    }

    /*
     * 当前底层连接是否已经建立。
     *
     * 注意：
     * - false 不一定代表不可用；
     * - 后续调用 ensureChannelReady() 可以自动触发 connect()。
     */
    [[nodiscard]] bool connected() const {
        return channel_ != nullptr && channel_->connected();
    }

    /*
     * 主动连接。
     *
     * 用户可以显式调用：
     *
     *   stub.connect();
     *
     * 也可以不调用，让具体 Stub 方法内部自动 ensureChannelReady()。
     */
    [[nodiscard]] novanet::rpc::RpcStatus connect() const {
        if (!channel_) {
            return nullChannelStatus();
        }

        return channel_->connect();
    }

protected:
    /*
     * 派生 Stub 使用这个接口访问 ClientChannel。
     *
     * 例如：
     *
     *   channel()->callUnary(...);
     */
    [[nodiscard]] const std::shared_ptr<ClientChannel>& channel() const noexcept {
        return channel_;
    }

    /*
     * 派生 Stub 在真正调用 RPC 前可以先调用这个函数。
     *
     * 作用：
     * - 检查 channel 是否为空；
     * - 如果未连接，则触发 ClientChannel::connect()；
     * - 返回 RpcStatus，方便业务 Stub 直接返回错误。
     */
    [[nodiscard]] novanet::rpc::RpcStatus ensureChannelReady() const {
        if (!channel_) {
            return nullChannelStatus();
        }

        if (channel_->connected()) {
            return novanet::rpc::RpcStatus::success();
        }

        return channel_->connect();
    }

    /*
     * 派生 Stub 可以复用这个错误。
     */
    [[nodiscard]] static novanet::rpc::RpcStatus nullChannelStatus() {
        return novanet::rpc::RpcStatus::failure(novanet::rpc::meta::RPC_BAD_REQUEST,
                                                "ClientChannel is null");
    }

private:
    std::shared_ptr<ClientChannel> channel_;
};

}  // namespace novanet::rpc::sdk