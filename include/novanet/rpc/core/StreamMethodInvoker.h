#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamMethodHandler.h"
#include "novanet/rpc/core/StreamResponder.h"

namespace novanet::rpc {

/*
 * StreamMethodInvoker 管理 server streaming method 的路由。
 *
 * ServiceRegistry 负责 protobuf service/method 是否存在；
 * StreamMethodInvoker 负责该 method 是否有 streaming handler。
 */
class StreamMethodInvoker final {
public:
    explicit StreamMethodInvoker(ServiceRegistry& registry);

    StreamMethodInvoker(const StreamMethodInvoker&) = delete;
    StreamMethodInvoker& operator=(const StreamMethodInvoker&) = delete;

    StreamMethodInvoker(StreamMethodInvoker&&) = delete;
    StreamMethodInvoker& operator=(StreamMethodInvoker&&) = delete;

    [[nodiscard]] bool registerHandler(StreamMethodHandler* handler,
                                       std::string* errorText = nullptr);

    [[nodiscard]] const StreamMethodHandler* findHandler(
        const std::string& serviceName, const std::string& methodName) const;

    [[nodiscard]] RpcStatus validate(const std::string& serviceName,
                                     const std::string& methodName) const;

    [[nodiscard]] RpcStatus start(std::uint32_t streamId,
                                  std::uint64_t requestId,
                                  const std::string& serviceName,
                                  const std::string& methodName,
                                  std::string requestPayload,
                                  std::shared_ptr<StreamResponder> responder);

    [[nodiscard]] std::size_t handlerCount() const noexcept;
    [[nodiscard]] std::vector<std::string> registeredMethods() const;

private:
    [[nodiscard]] static std::string makeKey(const std::string& serviceName,
                                             const std::string& methodName);

private:
    ServiceRegistry& registry_;
    std::unordered_map<std::string, StreamMethodHandler*> handlers_;
};

}  // namespace novanet::rpc
