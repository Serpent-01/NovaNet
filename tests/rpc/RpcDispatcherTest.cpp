#include <cassert>
#include <iostream>
#include <string>
#include <utility>
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
                controller->SetFailed(
                    "null argument in CalculatorServiceImpl::Add");
            }
            return;
        }

        response->set_result(request->lhs() + request->rhs());

        if (done != nullptr) {
            done->Run();
        }
    }
};

std::string serializeAddRequest(int lhs, int rhs) {
    ::novanet::example::calculator::AddRequest addRequest;
    addRequest.set_lhs(lhs);
    addRequest.set_rhs(rhs);

    std::string bytes;
    assert(addRequest.SerializeToString(&bytes));
    return bytes;
}

std::string makeUnaryRequestPayload(std::string serviceName,
                                    std::string methodName,
                                    std::string requestPayload) {
    novanet::rpc::UnaryRequestMeta requestMeta;
    requestMeta.set_service_name(std::move(serviceName));
    requestMeta.set_method_name(std::move(methodName));
    requestMeta.set_request_payload(std::move(requestPayload));

    std::string payload;
    assert(requestMeta.SerializeToString(&payload));
    return payload;
}

std::string makeAddUnaryRequestPayload(int lhs, int rhs) {
    return makeUnaryRequestPayload("CalculatorService", "Add",
                                   serializeAddRequest(lhs, rhs));
}

novanet::rpc::UnaryResponseMeta
parseUnaryResponse(const novanet::rpc::RpcMessage& responseMsg) {
    assert(responseMsg.valid());
    assert(responseMsg.frameType() == novanet::rpc::FrameType::UNARY_RESPONSE);

    novanet::rpc::UnaryResponseMeta responseMeta;
    assert(responseMeta.ParseFromString(responseMsg.payload()));

    return responseMeta;
}

} // namespace

int main() {
    using novanet::rpc::FrameType;
    using novanet::rpc::MethodInvoker;
    using novanet::rpc::RpcDispatcher;
    using novanet::rpc::RpcMessage;
    using novanet::rpc::ServiceRegistry;

    CalculatorServiceImpl calculator;

    ServiceRegistry registry;
    MethodInvoker invoker;
    novanet::rpc::StreamManager streamManager;
    novanet::rpc::StreamMethodInvoker streamInvoker(registry);

    std::string errorText;
    assert(registry.registerService(&calculator, &errorText));
    assert(errorText.empty());

    RpcDispatcher dispatcher(registry, invoker, streamInvoker);

    {
        /*
         * 测试 1：
         * 正常 unary 调用：
         *
         * UNARY_REQUEST CalculatorService.Add(1, 2)
         *      ↓
         * UNARY_RESPONSE RPC_OK + AddResponse{result=3}
         */
        RpcMessage requestMsg(FrameType::UNARY_REQUEST, 0, 1001,
                              makeAddUnaryRequestPayload(1, 2));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        const RpcMessage& responseMsg = responses[0];

        assert(responseMsg.valid());
        assert(responseMsg.frameType() == FrameType::UNARY_RESPONSE);
        assert(responseMsg.requestId() == 1001);
        assert(responseMsg.streamId() == 0);

        auto responseMeta = parseUnaryResponse(responseMsg);

        assert(responseMeta.error_code() == novanet::rpc::RPC_OK);
        assert(responseMeta.error_text().empty());
        assert(!responseMeta.response_payload().empty());

        ::novanet::example::calculator::AddResponse addResponse;
        assert(addResponse.ParseFromString(responseMeta.response_payload()));
        assert(addResponse.result() == 3);
    }

    {
        /*
         * 测试 2：
         * UnaryRequestMeta 本身无法反序列化。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_PARSE_REQUEST_FAILED
         */
        std::string badPayload;
        badPayload.push_back(static_cast<char>(0x0A));
        badPayload.push_back(static_cast<char>(0xFF));

        RpcMessage requestMsg(FrameType::UNARY_REQUEST, 0, 1002,
                              std::move(badPayload));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() ==
               novanet::rpc::RPC_PARSE_REQUEST_FAILED);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 3：
         * service_name 为空。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_BAD_REQUEST
         */
        RpcMessage requestMsg(
            FrameType::UNARY_REQUEST, 0, 1003,
            makeUnaryRequestPayload("", "Add", serializeAddRequest(1, 2)));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() == novanet::rpc::RPC_BAD_REQUEST);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 4：
         * method_name 为空。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_BAD_REQUEST
         */
        RpcMessage requestMsg(
            FrameType::UNARY_REQUEST, 0, 1004,
            makeUnaryRequestPayload("CalculatorService", "",
                                    serializeAddRequest(1, 2)));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() == novanet::rpc::RPC_BAD_REQUEST);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 5：
         * service 不存在。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_SERVICE_NOT_FOUND
         */
        RpcMessage requestMsg(
            FrameType::UNARY_REQUEST, 0, 1005,
            makeUnaryRequestPayload("NoSuchService", "Add",
                                    serializeAddRequest(1, 2)));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() ==
               novanet::rpc::RPC_SERVICE_NOT_FOUND);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 6：
         * method 不存在。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_METHOD_NOT_FOUND
         */
        RpcMessage requestMsg(
            FrameType::UNARY_REQUEST, 0, 1006,
            makeUnaryRequestPayload("CalculatorService", "NoSuchMethod",
                                    serializeAddRequest(1, 2)));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() == novanet::rpc::RPC_METHOD_NOT_FOUND);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 7：
         * UnaryRequestMeta 可以解析，
         * 但里面的 request_payload 不是合法 AddRequest。
         *
         * 这会进入 MethodInvoker，
         * 然后 MethodInvoker 返回 RPC_PARSE_REQUEST_FAILED。
         *
         * 期望：
         * UNARY_RESPONSE + RPC_PARSE_REQUEST_FAILED
         */
        std::string badAddRequestBytes;
        badAddRequestBytes.push_back(static_cast<char>(0x0A));
        badAddRequestBytes.push_back(static_cast<char>(0xFF));

        RpcMessage requestMsg(
            FrameType::UNARY_REQUEST, 0, 1007,
            makeUnaryRequestPayload("CalculatorService", "Add",
                                    std::move(badAddRequestBytes)));

        assert(requestMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(requestMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        auto responseMeta = parseUnaryResponse(responses[0]);

        assert(responseMeta.error_code() ==
               novanet::rpc::RPC_PARSE_REQUEST_FAILED);
        assert(!responseMeta.error_text().empty());
        assert(responseMeta.response_payload().empty());
    }

    {
        /*
         * 测试 8：
         * 企业级 Phase 4 dispatcher 支持 HEARTBEAT_PING。
         *
         * 期望：
         * HEARTBEAT_PONG
         */
        RpcMessage pingMsg(FrameType::HEARTBEAT_PING, 0, 0, {});

        assert(pingMsg.valid());

        std::vector<RpcMessage> responses;
        const bool ok =
            dispatcher.dispatch(pingMsg, streamManager, responses, nullptr);

        assert(ok);
        assert(responses.size() == 1);

        const RpcMessage& responseMsg = responses[0];

        assert(responseMsg.valid());
        assert(responseMsg.frameType() == FrameType::HEARTBEAT_PONG);
        assert(responseMsg.streamId() == 0);
        assert(responseMsg.requestId() == 0);
    }

    std::cout << "[PASS] RpcDispatcherTest passed.\n";
    return 0;
}
