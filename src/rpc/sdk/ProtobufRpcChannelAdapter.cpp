#include "novanet/rpc/sdk/ProtobufRpcChannelAdapter.h"

#include <google/protobuf/descriptor.h>

#include <utility>

#include "novanet/base/Logger.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc::sdk {

ProtobufRpcChannelAdapter::ProtobufRpcChannelAdapter(
    std::shared_ptr<ClientChannel> channel)
    : channel_(std::move(channel)) {
}

void ProtobufRpcChannelAdapter::CallMethod(
    const google::protobuf::MethodDescriptor* method,
    google::protobuf::RpcController* controller,
    const google::protobuf::Message* request, google::protobuf::Message* response,
    google::protobuf::Closure* done) {
    /*
     * protobuf RpcChannel 约定：
     * - done 可以为空；
     * - 如果 done 非空，调用结束后应该执行 done->Run()；
     * - 这里是同步调用 NovaNet unary RPC，最后再执行 done。
     */

    if (!channel_) {
        const std::string error = "ProtobufRpcChannelAdapter has null ClientChannel";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error;
        runDone(done);
        return;
    }

    if (method == nullptr) {
        const std::string error = "protobuf MethodDescriptor is null";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error;
        runDone(done);
        return;
    }

    if (request == nullptr) {
        const std::string error = "protobuf request message is null";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error
                  << ", method=" << methodFullName(method);
        runDone(done);
        return;
    }

    if (response == nullptr) {
        const std::string error = "protobuf response message is null";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error
                  << ", method=" << methodFullName(method);
        runDone(done);
        return;
    }

    if (controllerCancelled(controller)) {
        const std::string error = "protobuf RPC cancelled before send";
        setControllerFailed(controller, error);
        LOG_WARN << "[ProtobufRpcChannelAdapter] " << error
                 << ", method=" << methodFullName(method);
        runDone(done);
        return;
    }

    const auto* service = method->service();
    if (service == nullptr) {
        const std::string error = "protobuf service descriptor is null";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error;
        runDone(done);
        return;
    }

    const std::string serviceName = service->full_name();
    const std::string methodName = method->name();

    if (serviceName.empty() || methodName.empty()) {
        const std::string error = "protobuf service or method name is empty";
        setControllerFailed(controller, error);
        LOG_ERROR << "[ProtobufRpcChannelAdapter] " << error;
        runDone(done);
        return;
    }

    /*
     * google::protobuf::RpcController 本身没有通用 metadata / timeout 接口。
     * 所以这里创建一个本地 ClientContext，使用 ClientChannel / ChannelOptions
     * 的默认 RPC timeout。
     *
     * 如果后续要让 protobuf stub 支持 metadata/deadline，可以自定义一个
     * 继承 google::protobuf::RpcController 的 NovaNetClientController，
     * 再在这里 dynamic_cast 读取 metadata/deadline。
     */
    ClientContext ctx;

    novanet::rpc::RpcStatus status =
        channel_->callUnary(serviceName, methodName, *request, response, &ctx);

    if (!status.ok()) {
        setControllerFailed(controller, status.toString());

        LOG_WARN << "[ProtobufRpcChannelAdapter] call failed, method=" << serviceName
                 << "." << methodName << ", status=" << status.toString();

        runDone(done);
        return;
    }

    LOG_INFO << "[ProtobufRpcChannelAdapter] call ok, method=" << serviceName << "."
             << methodName;

    runDone(done);
}

void ProtobufRpcChannelAdapter::runDone(google::protobuf::Closure* done) {
    if (done != nullptr) {
        done->Run();
    }
}

void ProtobufRpcChannelAdapter::setControllerFailed(
    google::protobuf::RpcController* controller, const std::string& errorText) {
    if (controller != nullptr) {
        controller->SetFailed(errorText);
    }
}

bool ProtobufRpcChannelAdapter::controllerCancelled(
    google::protobuf::RpcController* controller) {
    return controller != nullptr && controller->IsCanceled();
}

std::string ProtobufRpcChannelAdapter::methodFullName(
    const google::protobuf::MethodDescriptor* method) {
    if (method == nullptr) {
        return "<null method>";
    }

    const auto* service = method->service();
    if (service == nullptr) {
        return method->name();
    }

    return service->full_name() + "." + method->name();
}

}  // namespace novanet::rpc::sdk