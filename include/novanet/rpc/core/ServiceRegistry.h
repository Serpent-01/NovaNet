#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace google {
namespace protobuf {

class Service;
class ServiceDescriptor;
class MethodDescriptor;

}  // namespace protobuf
}  // namespace google

namespace novanet::rpc {

class ServiceRegistry final {
public:
    struct MethodMeta final {
        std::string name;
        std::string fullName;

        // 不拥有 MethodDescriptor 生命周期。
        // MethodDescriptor 由 protobuf 生成代码和 DescriptorPool 管理。
        const google::protobuf::MethodDescriptor* method{nullptr};
        [[nodiscard]] bool valid() const noexcept {
            return method != nullptr;
        }
    };
    struct ServiceMeta final {
        std::string name;
        std::string fullName;

        // 不拥有 service 生命周期。
        // 第一版约定：service 对象由 RpcServer 或用户代码持有，
        // ServiceRegistry 只保存裸指针索引用于分发。
        const google::protobuf::Service* service{nullptr};

        // 不拥有 descriptor 生命周期。
        const google::protobuf::ServiceDescriptor* descriptor{nullptr};

        std::unordered_map<std::string, MethodMeta> methods;

        [[nodiscard]] bool valid() const noexcept {
            return service != nullptr && descriptor != nullptr;
        }
    };

public:
    ServiceRegistry() = default;

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;

    ~ServiceRegistry() = default;

    /*
     * 注册一个 protobuf service。
     *
     * 返回 false 的情况：
     * 1. service == nullptr
     * 2. service->GetDescriptor() == nullptr
     * 3. service full_name 重复注册
     * 4. service 没有任何 method
     *
     * 生命周期约定：
     * ServiceRegistry 不拥有 service。
     * 调用者必须保证 service 的生命周期长于 ServiceRegistry 的使用期。
     */
    [[nodiscard]] bool registerService(google::protobuf::Service* service,
                                       std::string* errorText = nullptr);
    /*
     * 根据 service name 查找服务。
     *
     * 支持两种名字：
     * 1. full name:  "novanet.example.CalculatorService"
     * 2. short name: "CalculatorService"
     *
     * 如果 short name 冲突，则 short name 查找会失败。
     */
    [[nodiscard]] const ServiceMeta* findService(
        const std::string& serviceName) const;

    /*
     * 在某个 service 内查找 method。
     *
     * 支持：
     * 1. short name: "Add"
     * 2. full name:  "novanet.example.CalculatorService.Add"
     */
    [[nodiscard]] const MethodMeta* findMethod(
        const ServiceMeta& serviceMeta, const std::string& methodName) const;

    /*
     * 组合查找：
     *
     * service_name + method_name -> MethodMeta
     */
    [[nodiscard]] const MethodMeta* findMethod(
        const std::string& serviceName, const std::string& methodName) const;

    [[nodiscard]] std::size_t serviceCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    // key: service full name，例如 "novanet.example.CalculatorService"
    std::unordered_map<std::string, ServiceMeta> services_;

    /*
     * short name -> full name
     *
     * 例如：
     *   "CalculatorService" -> "novanet.example.CalculatorService"
     *
     * 如果出现 short name 冲突，value 会被置空字符串，
     * 表示这个 short name 是 ambiguous，必须使用 full name 查找。
     */
    std::unordered_map<std::string, std::string> serviceAliases_;
};
}  // namespace novanet::rpc
