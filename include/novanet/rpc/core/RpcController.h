#pragma once

#include <google/protobuf/service.h>

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
    /*
     * google::protobuf::RpcController 接口。
     *
     * Protobuf 生成的 service method 接收的是：
     *
     *     google::protobuf::RpcController*
     *
     * 所以 NovaNet 的 RpcController 需要实现这些接口。
     */
};

}  // namespace novanet::rpc