#include "novanet/rpc/core/MethodInvoker.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <memory.h>

#include <string>
#include <utility>

#include "novanet/rpc/core/RpcController.h"

namespace novanet::rpc {
namespace {
bool fail(std::string &errorText, std::string message) {
    errorText = std::move(message);
    return false;
}

}  // namespace

bool MethodInvoker::invokeUnary(const ServiceRegistry::ServiceMeta &serviceMeta,
                                const ServiceRegistry::MethodMeta &methodMeta,
                                const std::string &requestBytes,
                                std::string &responseBytes,
                                std::string &errorText) const {
    // 1. 初始化输出缓冲区与异常状态
    responseBytes.clear();
    errorText.clear();

    // 2. 校验元数据合法性与空指针防卫 (Defensive Programming)
    if (!serviceMeta.valid()) {
        return fail(errorText, "invalid service meta");
    }
    if (!methodMeta.valid()) {
        return fail(errorText, "invalid method meta");
    }

    auto *service = serviceMeta.service;
    const auto *method = methodMeta.method;

    if (service == nullptr) {
        return fail(errorText, "protobuf service is null");
    }
    if (method == nullptr) {
        return fail(errorText, "protobuf method descriptor is null");
    }
    if (serviceMeta.descriptor == nullptr) {
        return fail(errorText, "protobuf service descriptor is null");
    }

    // 3. 元数据血缘校验：利用 Protobuf Descriptor 全局单例特性的 O(1)
    // 指针比对，防止非本服务 Method 越权挂载
    if (method->service() != serviceMeta.descriptor) {
        return fail(
            errorText,
            "method does not belong to service: method=" + method->full_name() +
                ", service=" + serviceMeta.descriptor->full_name());
    }

    // 4. 请求反序列化：基于 Prototype 模式动态实例化强类型 Request
    // Message，完成 Data Plane 数据填充
    std::unique_ptr<google::protobuf::Message> request(
        service->GetRequestPrototype(method).New());
    if (!request) {
        return fail(errorText, "failed to create request message for method: " +
                                   method->full_name());
    }
    if (!request->ParseFromString(requestBytes)) {
        return fail(errorText, "failed to parse request payload for method: " +
                                   method->full_name());
    }

    // 5. 响应内存分配：基于 Prototype 模式动态分配全新的 Response Message
    // 实例用于承载业务 Payload
    std::unique_ptr<google::protobuf::Message> response(
        service->GetResponsePrototype(method).New());
    if (!response) {
        return fail(errorText,
                    "failed to create response message for method: " +
                        method->full_name());
    }

    // 6. 业务路由分发：实例化当前 RPC 请求专属的 Control Plane (RpcController)
    // 上下文。 注：在 Unary (同步请求-响应) 语义下，不涉及异步回调
    // Closure置为 nullptr。
    RpcController controller;
    service->CallMethod(method, &controller, request.get(), response.get(),
                        nullptr);

    // 7. 异常收敛检查：探查 Control Plane 是否捕获到业务层抛出的逻辑异常
    if (controller.failed()) {
        return fail(errorText,
                    "method invoke failed: " + controller.errorText());
    }

    // 8. 响应序列化：业务流转成功，将 Response
    // 强类型对象编码为二进制字节流，交由底层网络传输
    if (!response->SerializeToString(&responseBytes)) {
        responseBytes.clear();
        return fail(errorText, "failed to serialize response for method: " +
                                   method->full_name());
    }

    return true;
}

}  // namespace novanet::rpc