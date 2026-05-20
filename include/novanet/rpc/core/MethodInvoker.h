#pragma once

#include <string>

#include "novanet/rpc/core/ServiceRegistry.h"
namespace novanet::rpc {
/*
 * MethodInvoker 负责真正执行一次 unary protobuf service 调用。
 *
 * 它位于：
 *
 *     RpcDispatcher
 *         ↓
 *     ServiceRegistry
 *         ↓
 *     MethodInvoker
 *
 * 职责：
 * 1. 根据 MethodDescriptor 创建 request protobuf 对象
 * 2. 从 requestBytes 反序列化 request
 * 3. 根据 MethodDescriptor 创建 response protobuf 对象
 * 4. 调用 google::protobuf::Service::CallMethod()
 * 5. 将 response 序列化成 responseBytes
 *
 * 不负责：
 * - 不解析 RpcMessage
 * - 不解析 UnaryRequestMeta
 * - 不操作 socket / Buffer
 * - 不管理 request_id
 * - 不生成 RpcMessage
 *
 * 第一版采用同步调用，不引入异步 Closure。
 */

class MethodInvoker final {
public:
    MethodInvoker() = default;

    MethodInvoker(const MethodInvoker&) = delete;
    MethodInvoker& operator=(const MethodInvoker&) = delete;

    MethodInvoker(MethodInvoker&&) = default;
    MethodInvoker& operator=(MethodInvoker&&) = default;

    ~MethodInvoker() = default;

    /*
     * 执行一次 unary RPC 调用。
     *
     * 参数：
     * - serviceMeta:
     *      ServiceRegistry 查到的服务元信息，里面包含 protobuf service 指针。
     *
     * - methodMeta:
     *      ServiceRegistry 查到的方法元信息，里面包含 MethodDescriptor。
     *
     * - requestBytes:
     *      业务 request protobuf 的序列化结果。
     *      例如 AddRequest.SerializeToString() 后的 bytes。
     *
     * - responseBytes:
     *      调用成功后，写入业务 response protobuf 的序列化结果。
     *      例如 AddResponse.SerializeToString() 后的 bytes。
     *
     * - errorText:
     *      调用失败后，写入错误原因。
     *
     * 返回：
     * - true:
     *      调用成功，responseBytes 有效。
     *
     * - false:
     *      调用失败，errorText 有效。
     */
    [[nodiscard]] bool invokeUnary(
        const ServiceRegistry::ServiceMeta& serviceMeta,
        const ServiceRegistry::MethodMeta& methodMeta,
        const std::string& requestBytes, std::string& responseBytes,
        std::string& errorText) const;
};
}  // namespace novanet::rpc