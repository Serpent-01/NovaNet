#include "novanet/rpc/core/StreamMethodInvoker.h"

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

StreamMethodInvoker::StreamMethodInvoker(ServiceRegistry& registry)
    : registry_(registry) {
}

bool StreamMethodInvoker::registerHandler(StreamMethodHandler* handler,
                                          std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    if (handler == nullptr) {
        return fail(errorText, "stream method handler is null");
    }

    const std::string serviceName = handler->serviceName();
    const std::string methodName = handler->methodName();

    if (serviceName.empty() || methodName.empty()) {
        return fail(errorText, "stream method handler has empty service/method");
    }

    const std::string key = makeKey(serviceName, methodName);
    auto [it, inserted] = handlers_.emplace(key, handler);
    if (!inserted) {
        return fail(errorText,
                    "stream method handler already registered: " + serviceName +
                        "." + methodName);
    }

    return true;
}

const StreamMethodHandler* StreamMethodInvoker::findHandler(
    const std::string& serviceName, const std::string& methodName) const {
    if (serviceName.empty() || methodName.empty()) {
        return nullptr;
    }

    const auto it = handlers_.find(makeKey(serviceName, methodName));
    if (it == handlers_.end()) {
        return nullptr;
    }

    return it->second;
}

RpcStatus StreamMethodInvoker::validate(const std::string& serviceName,
                                        const std::string& methodName) const {
    if (serviceName.empty() || methodName.empty()) {
        return RpcStatus::failure(meta::RPC_BAD_REQUEST,
                                  "stream service_name or method_name is empty");
    }

    const auto* serviceMeta = registry_.findService(serviceName);
    if (serviceMeta == nullptr) {
        return RpcStatus::failure(meta::RPC_SERVICE_NOT_FOUND,
                                  "service not found: " + serviceName);
    }

    const auto* methodMeta = registry_.findMethod(*serviceMeta, methodName);
    if (methodMeta == nullptr) {
        return RpcStatus::failure(meta::RPC_METHOD_NOT_FOUND,
                                  "method not found: " + serviceName + "." +
                                      methodName);
    }

    if (findHandler(serviceName, methodName) == nullptr) {
        return RpcStatus::failure(meta::RPC_METHOD_NOT_FOUND,
                                  "stream method handler not registered: " +
                                      serviceName + "." + methodName);
    }

    return RpcStatus::success();
}

RpcStatus StreamMethodInvoker::start(
    std::uint32_t streamId, std::uint64_t requestId,
    const std::string& serviceName, const std::string& methodName,
    std::string requestPayload, std::shared_ptr<StreamResponder> responder) {
    if (streamId == 0 || requestId == 0) {
        return RpcStatus::failure(meta::RPC_BAD_REQUEST,
                                  "streamId/requestId must be non-zero");
    }

    if (!responder) {
        return RpcStatus::failure(meta::RPC_INTERNAL_ERROR,
                                  "StreamResponder is null");
    }

    const RpcStatus status = validate(serviceName, methodName);
    if (status.failed()) {
        return status;
    }

    StreamMethodHandler* handler =
        handlers_.find(makeKey(serviceName, methodName))->second;

    StreamMethodHandler::StartContext context;
    context.streamId = streamId;
    context.requestId = requestId;
    context.requestPayload = std::move(requestPayload);
    context.responder = std::move(responder);

    return handler->start(std::move(context));
}

std::size_t StreamMethodInvoker::handlerCount() const noexcept {
    return handlers_.size();
}

std::vector<std::string> StreamMethodInvoker::registeredMethods() const {
    std::vector<std::string> methods;
    methods.reserve(handlers_.size());

    for (const auto& item : handlers_) {
        methods.push_back(item.first);
    }

    return methods;
}

std::string StreamMethodInvoker::makeKey(const std::string& serviceName,
                                         const std::string& methodName) {
    std::string key;
    key.reserve(serviceName.size() + 1 + methodName.size());
    key.append(serviceName);
    key.push_back('\n');
    key.append(methodName);
    return key;
}

}  // namespace novanet::rpc
