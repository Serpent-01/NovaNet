#include <cassert>
#include <iostream>
#include <string>

#include "calculator.pb.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/ServiceRegistry.h"
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

class FailingCalculatorServiceImpl final
    : public ::novanet::example::calculator::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::calculator::AddRequest* request,
             ::novanet::example::calculator::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        (void)request;
        (void)response;

        if (controller != nullptr) {
            controller->SetFailed("intentional failure");
        }

        if (done != nullptr) {
            done->Run();
        }
    }
};

} // namespace

int main() {
    using novanet::rpc::MethodInvoker;
    using novanet::rpc::ServiceRegistry;

    {
        CalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));
        assert(errorText.empty());

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        ::novanet::example::calculator::AddRequest request;
        request.set_lhs(1);
        request.set_rhs(2);

        std::string requestBytes;
        assert(request.SerializeToString(&requestBytes));

        MethodInvoker invoker;

        const auto result =
            invoker.invokeUnary(*serviceMeta, *methodMeta, requestBytes);

        assert(result.ok());
        assert(result.errorCode() == novanet::rpc::RPC_OK);
        assert(result.errorText().empty());
        assert(!result.responseBytes().empty());

        ::novanet::example::calculator::AddResponse response;
        assert(response.ParseFromString(result.responseBytes()));
        assert(response.result() == 3);
    }

    {
        CalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        MethodInvoker invoker;

        std::string badRequestBytes;
        badRequestBytes.push_back(static_cast<char>(0x0A));
        badRequestBytes.push_back(static_cast<char>(0xFF));

        const auto result =
            invoker.invokeUnary(*serviceMeta, *methodMeta, badRequestBytes);

        assert(result.failed());
        assert(result.errorCode() == novanet::rpc::RPC_PARSE_REQUEST_FAILED);
        assert(result.responseBytes().empty());
        assert(!result.errorText().empty());
    }

    {
        FailingCalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        ::novanet::example::calculator::AddRequest request;
        request.set_lhs(10);
        request.set_rhs(20);

        std::string requestBytes;
        assert(request.SerializeToString(&requestBytes));

        MethodInvoker invoker;

        const auto result =
            invoker.invokeUnary(*serviceMeta, *methodMeta, requestBytes);

        assert(result.failed());
        assert(result.errorCode() == novanet::rpc::RPC_INVOKE_FAILED);
        assert(result.responseBytes().empty());
        assert(result.errorText().find("intentional failure") !=
               std::string::npos);
    }

    std::cout << "[PASS] MethodInvokerTest passed.\n";
    return 0;
}