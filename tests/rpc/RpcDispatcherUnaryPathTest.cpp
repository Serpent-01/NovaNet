#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "calculator.pb.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/RpcDispatcher.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamMethodInvoker.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "novanet/rpc/stream/StreamManager.h"
#include "rpc_meta.pb.h"

namespace {
class CalculatorServiceImpl final
    : public ::novanet::example::calculator::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::calculator::AddRequest* request,
             ::novanet::example::calculator::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        if (controller == nullptr || request == nullptr ||
            response == nullptr) {
            if (controller != nullptr) {
                controller->SetFailed("null argument in Add");
            }
            return;
        }
        response->set_result(request->lhs() + request->rhs());
        if (done != nullptr) {
            done->Run();
        }
    }
};
} // namespace

int main() {
    using novanet::rpc::FrameType;
    using novanet::rpc::MethodInvoker;
    using novanet::rpc::RpcDispatcher;
    using novanet::rpc::RpcMessage;
    using novanet::rpc::ServiceRegistry;

    CalculatorServiceImpl calculator;

    ServiceRegistry registry;
    std::string registerError;
    const bool registered =
        registry.registerService(&calculator, &registerError);
    assert(registered);
    assert(registerError.empty());

    MethodInvoker invoker;
    novanet::rpc::StreamManager streamManager;
    novanet::rpc::StreamMethodInvoker streamInvoker(registry);
    RpcDispatcher dispatcher(registry, invoker, streamInvoker);

    ::novanet::example::calculator::AddRequest addRequest;
    addRequest.set_lhs(1);
    addRequest.set_rhs(2);
    std::string addRequestBytes;
    assert(addRequest.SerializeToString(&addRequestBytes));
    assert(!addRequestBytes.empty());
    novanet::rpc::UnaryRequestMeta requestMeta;
    requestMeta.set_service_name("CalculatorService");
    requestMeta.set_method_name("Add");
    requestMeta.set_request_payload(std::move(addRequestBytes));

    std::string rpcPayload;
    assert(requestMeta.SerializeToString(&rpcPayload));
    assert(!rpcPayload.empty());

    RpcMessage requestMsg(FrameType::UNARY_REQUEST,
                          0,    // unary 第一版 streamId 可以为 0
                          1001, // requestId 必须非 0
                          std::move(rpcPayload));
    assert(requestMsg.valid());
    assert(requestMsg.frameType() == FrameType::UNARY_REQUEST);
    assert(requestMsg.requestId() == 1001);

    std::vector<RpcMessage> responses;
    const bool dispatched =
        dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);
    assert(dispatched);
    assert(responses.size() == 1);

    const RpcMessage& responseMsg = responses[0];

    assert(responseMsg.valid());
    assert(responseMsg.frameType() == FrameType::UNARY_RESPONSE);
    assert(responseMsg.requestId() == 1001);
    assert(responseMsg.streamId() == 0);

    novanet::rpc::UnaryResponseMeta responseMeta;
    assert(responseMeta.ParseFromString(responseMsg.payload()));

    assert(responseMeta.error_code() == novanet::rpc::RPC_OK);
    assert(responseMeta.error_text().empty());
    assert(!responseMeta.response_payload().empty());

    ::novanet::example::calculator::AddResponse addResponse;
    assert(addResponse.ParseFromString(responseMeta.response_payload()));

    assert(addResponse.result() == 3);

    std::cout << "[PASS] RpcDispatcherUnaryPathTest passed.\n";
    return 0;
}
