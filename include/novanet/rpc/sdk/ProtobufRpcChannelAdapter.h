#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <memory>
#include <string>

#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ClientChannel.h"
#include "novanet/rpc/sdk/ClientContext.h"

namespace novanet::rpc::sdk {

/*
 * ProtobufRpcChannelAdapter 把 google::protobuf::RpcChannel 接口
 * 适配到 NovaNet ClientChannel。
 *
 * 使用场景：
 *
 *   auto channel = novanet::rpc::CreateChannel("127.0.0.1:19090");
 *
 *   ProtobufRpcChannelAdapter adapter(channel);
 *
 *   novanet::example::calculator::CalculatorService_Stub stub(&adapter);
 *   stub.Add(controller, &req, &resp, done);
 *
 * 适用范围：
 * - 只适合 unary RPC；
 * - 用于 protobuf 生成的 *_Stub；
 * - 不适合 NovaNet 自定义 server streaming。
 *
 * 不做：
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试；
 * - 不做认证；
 * - 不做拦截器；
 * - 不直接访问 RpcClient / RpcChannel / TcpConnection。
 */
class ProtobufRpcChannelAdapter final : public google::protobuf::RpcChannel {
public:
    explicit ProtobufRpcChannelAdapter(std::shared_ptr<ClientChannel> channel);

    ~ProtobufRpcChannelAdapter() override = default;

    ProtobufRpcChannelAdapter(const ProtobufRpcChannelAdapter&) = delete;
    ProtobufRpcChannelAdapter& operator=(const ProtobufRpcChannelAdapter&) = delete;

    ProtobufRpcChannelAdapter(ProtobufRpcChannelAdapter&&) = delete;
    ProtobufRpcChannelAdapter& operator=(ProtobufRpcChannelAdapter&&) = delete;

    /*
     * protobuf generated Stub 会调用这个函数。
     *
     * 例如：
     *
     *   CalculatorService_Stub::Add(...)
     *       ↓
     *   ProtobufRpcChannelAdapter::CallMethod(...)
     */
    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(channel_);
    }

    [[nodiscard]] const std::shared_ptr<ClientChannel>& channel() const noexcept {
        return channel_;
    }

private:
    static void runDone(google::protobuf::Closure* done);

    static void setControllerFailed(google::protobuf::RpcController* controller,
                                    const std::string& errorText);

    static bool controllerCancelled(google::protobuf::RpcController* controller);

    static std::string methodFullName(
        const google::protobuf::MethodDescriptor* method);

private:
    std::shared_ptr<ClientChannel> channel_;
};

}  // namespace novanet::rpc::sdk