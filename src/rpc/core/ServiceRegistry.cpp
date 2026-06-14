#include "novanet/rpc/core/ServiceRegistry.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/service.h>

#include <utility>

namespace novanet::rpc {
namespace {

bool fail(std::string* errorText, std::string message) {
    if (errorText != nullptr) {
        *errorText = std::move(message);
    }
    return false;
}

}  // namespace

bool ServiceRegistry::registerService(google::protobuf::Service* service,
                                      std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    if (service == nullptr) {
        return fail(errorText, "cannot register null protobuf service");
    }

    //拿到服务的描述符
    const auto* descriptor = service->GetDescriptor();
    if (descriptor == nullptr) {
        return fail(errorText, "protobuf service descriptor is null");
    }

    const std::string fullName = descriptor->full_name();
    const std::string shortName = descriptor->name();

    if (fullName.empty()) {
        return fail(errorText, "protobuf service full_name is empty");
    }

    if (shortName.empty()) {
        return fail(errorText, "protobuf service name is empty");
    }

    if (services_.find(fullName) != services_.end()) {
        return fail(errorText,
                    "protobuf service already registered: " + fullName);
    }

    if (descriptor->method_count() <= 0) {
        return fail(errorText, "protobuf service has no methods: " + fullName);
    }

    ServiceMeta serviceMeta;
    serviceMeta.name = shortName;
    serviceMeta.fullName = fullName;
    serviceMeta.service = service;
    serviceMeta.descriptor = descriptor;

    for (int i = 0; i < descriptor->method_count(); ++i) {
        const auto* method = descriptor->method(i);
        if (method == nullptr) {
            return fail(
                errorText,
                "protobuf method descriptor is null in service: " + fullName);
        }

        MethodMeta methodMeta;
        methodMeta.name = method->name();
        methodMeta.fullName = method->full_name();
        methodMeta.method = method;

        if (methodMeta.name.empty()) {
            return fail(
                errorText,
                "protobuf method name is empty in service: " + fullName);
        }

        const auto inserted =
            serviceMeta.methods.emplace(methodMeta.name, std::move(methodMeta));

        if (!inserted.second) {
            return fail(errorText, "duplicated method name in service: " +
                                       fullName + "." + inserted.first->first);
        }
    }

    auto insertedService = services_.emplace(fullName, std::move(serviceMeta));
    if (!insertedService.second) {
        return fail(errorText, "failed to insert service: " + fullName);
    }

    /*
     * short name alias 策略：
     *
     * 如果短名没有冲突：
     *   "CalculatorService" -> "novanet.example.CalculatorService"
     *
     * 如果短名冲突：
     *   "CalculatorService" -> ""
     *
     * 这样 findService("CalculatorService") 会失败，
     * 调用方必须使用 full name，避免错误路由。
     */
    auto aliasIt = serviceAliases_.find(shortName);
    if (aliasIt == serviceAliases_.end()) {
        serviceAliases_.emplace(shortName, fullName);
    } else if (aliasIt->second != fullName) {
        aliasIt->second.clear();
    }

    return true;
}

const ServiceRegistry::ServiceMeta* ServiceRegistry::findService(
    const std::string& serviceName) const {
    if (serviceName.empty()) {
        return nullptr;
    }

    // 1. 优先按 full name 查找。
    auto serviceIt = services_.find(serviceName);
    if (serviceIt != services_.end()) {
        return &serviceIt->second;
    }
    // 2. 再按 short name alias 查找。
    auto aliasIt = serviceAliases_.find(serviceName);
    if (aliasIt == serviceAliases_.end()) {
        return nullptr;
    }

    // short name 冲突，必须使用 full name。
    if (aliasIt->second.empty()) {
        return nullptr;
    }

    serviceIt = services_.find(aliasIt->second);
    if (serviceIt == services_.end()) {
        return nullptr;
    }
    return &serviceIt->second;
}

const ServiceRegistry::MethodMeta* ServiceRegistry::findMethod(
    const ServiceMeta& serviceMeta, const std::string& methodName) const {
    if (!serviceMeta.valid() || methodName.empty()) {
        return nullptr;
    }
    // 1. 优先按 method short name 查找，例如 "Add"。
    auto methodIt = serviceMeta.methods.find(methodName);
    if (methodIt != serviceMeta.methods.end()) {
        return &methodIt->second;
    }
    // 2. 兼容 method full name，例如 "novanet.example.CalculatorService.Add"。
    for (const auto& item : serviceMeta.methods) {
        const auto& methodMeta = item.second;
        if (methodMeta.fullName == methodName) {
            return &methodMeta;
        }
    }
    return nullptr;
}

const ServiceRegistry::MethodMeta* ServiceRegistry::findMethod(
    const std::string& serviceName, const std::string& methodName) const {
    const ServiceMeta* serviceMeta = findService(serviceName);
    if (serviceMeta == nullptr) {
        return nullptr;
    }
    return findMethod(*serviceMeta, methodName);
}

std::size_t ServiceRegistry::serviceCount() const noexcept {
    return services_.size();
}
bool ServiceRegistry::empty() const noexcept {
    return services_.empty();
}

}  // namespace novanet::rpc