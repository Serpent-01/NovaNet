#include "novanet/rpc/core/MethodInvoker.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <memory>
#include <string>

#include "novanet/rpc/core/RpcController.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

MethodInvokeResult MethodInvoker::invokeUnary(
    const ServiceRegistry::ServiceMeta& serviceMeta,
    const ServiceRegistry::MethodMeta& methodMeta,
    const std::string& requestBytes) const {
    if (!serviceMeta.valid()) {
        return MethodInvokeResult::failure(RPC_BAD_REQUEST,
                                           "invalid service meta");
    }

    if (!methodMeta.valid()) {
        return MethodInvokeResult::failure(RPC_BAD_REQUEST,
                                           "invalid method meta");
    }

    auto* service = serviceMeta.service;
    const auto* method = methodMeta.method;

    if (service == nullptr) {
        return MethodInvokeResult::failure(RPC_BAD_REQUEST,
                                           "protobuf service is null");
    }

    if (method == nullptr) {
        return MethodInvokeResult::failure(
            RPC_BAD_REQUEST, "protobuf method descriptor is null");
    }

    if (serviceMeta.descriptor == nullptr) {
        return MethodInvokeResult::failure(
            RPC_BAD_REQUEST, "protobuf service descriptor is null");
    }

    /*
     * 防御性校验：
     * method 必须属于当前 service。
     */
    if (method->service() != serviceMeta.descriptor) {
        return MethodInvokeResult::failure(
            RPC_BAD_REQUEST,
            "method does not belong to service: method=" + method->full_name() +
                ", service=" + serviceMeta.descriptor->full_name());
    }

    /*
     * 根据 MethodDescriptor 创建正确类型的 request。
     *
     * 框架层并不知道具体类型是不是 AddRequest，
     * 但是 protobuf service + method descriptor 知道。
     */
    std::unique_ptr<google::protobuf::Message> request(
        service->GetRequestPrototype(method).New());

    if (!request) {
        return MethodInvokeResult::failure(
            RPC_UNKNOWN_ERROR, "failed to create request message for method: " +
                                   method->full_name());
    }

    if (!request->ParseFromString(requestBytes)) {
        return MethodInvokeResult::failure(
            RPC_PARSE_REQUEST_FAILED,
            "failed to parse request payload for method: " +
                method->full_name());
    }

    /*
     * 创建正确类型的 response。
     */
    std::unique_ptr<google::protobuf::Message> response(
        service->GetResponsePrototype(method).New());

    if (!response) {
        return MethodInvokeResult::failure(
            RPC_UNKNOWN_ERROR,
            "failed to create response message for method: " +
                method->full_name());
    }

    RpcController controller;

    service->CallMethod(method, &controller, request.get(), response.get(),
                        nullptr);

    if (controller.failed()) {
        return MethodInvokeResult::failure(
            RPC_INVOKE_FAILED,
            "method invoke failed: " + controller.errorText());
    }

    std::string responseBytes;
    if (!response->SerializeToString(&responseBytes)) {
        return MethodInvokeResult::failure(
            RPC_SERIALIZE_RESPONSE_FAILED,
            "failed to serialize response for method: " + method->full_name());
    }

    return MethodInvokeResult::success(std::move(responseBytes));
}

}  // namespace novanet::rpc