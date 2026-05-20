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
    responseBytes.clear();
    errorText.clear();

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
    if (method->service() != serviceMeta.descriptor) {
        return fail(
            errorText,
            "method does not belong to service: method=" + method->full_name() +
                ", service=" + serviceMeta.descriptor->full_name());
    }

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

    std::unique_ptr<google::protobuf::Message> response(
        service->GetResponsePrototype(method).New());

    if (!response) {
        return fail(errorText,
                    "failed to create response message for method: " +
                        method->full_name());
    }

    RpcController controller;
    service->CallMethod(method, &controller, request.get(), response.get(),
                        nullptr);

    if (controller.failed()) {
        return fail(errorText,
                    "method invoke failed: " + controller.errorText());
    }

    if (!response->SerializeToString(&responseBytes)) {
        responseBytes.clear();
        return fail(errorText, "failed to serialize response for method: " +
                                   method->full_name());
    }
    return true;
}

}  // namespace novanet::rpc