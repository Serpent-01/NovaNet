#include <cassert>
#include <iostream>
#include <string>

#include "calculator.pb.h"
#include "novanet/rpc/core/MethodInvoker.h"
#include "novanet/rpc/core/ServiceRegistry.h"

namespace {

class CalculatorServiceImpl final : public novanet::example::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::AddRequest* request,
             ::novanet::example::AddResponse* response,
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
    : public novanet::example::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::AddRequest* request,
             ::novanet::example::AddResponse* response,
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

}  // namespace

int main() {
    using novanet::rpc::MethodInvoker;
    using novanet::rpc::ServiceRegistry;

    {
        /*
         * 测试 1：
         * 正常 Add(1,2) -> 3。
         */
        CalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));
        assert(errorText.empty());

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        novanet::example::AddRequest request;
        request.set_lhs(1);
        request.set_rhs(2);

        std::string requestBytes;
        assert(request.SerializeToString(&requestBytes));
        assert(!requestBytes.empty());

        MethodInvoker invoker;

        std::string responseBytes;
        errorText.clear();

        const bool ok = invoker.invokeUnary(
            *serviceMeta, *methodMeta, requestBytes, responseBytes, errorText);

        assert(ok);
        assert(errorText.empty());
        assert(!responseBytes.empty());

        novanet::example::AddResponse response;
        assert(response.ParseFromString(responseBytes));
        assert(response.result() == 3);
    }

    {
        /*
         * 测试 2：
         * requestBytes 是非法 protobuf，应该失败。
         */
        CalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        MethodInvoker invoker;

        std::string responseBytes;
        errorText.clear();

        const std::string badRequestBytes =
            "this is not a valid AddRequest protobuf";

        const bool ok =
            invoker.invokeUnary(*serviceMeta, *methodMeta, badRequestBytes,
                                responseBytes, errorText);

        assert(!ok);
        assert(responseBytes.empty());
        assert(!errorText.empty());
    }

    {
        /*
         * 测试 3：
         * 业务方法通过 controller->SetFailed() 主动标记失败。
         */
        FailingCalculatorServiceImpl calculator;

        ServiceRegistry registry;
        std::string errorText;

        assert(registry.registerService(&calculator, &errorText));

        const auto* serviceMeta = registry.findService("CalculatorService");
        assert(serviceMeta != nullptr);

        const auto* methodMeta = registry.findMethod(*serviceMeta, "Add");
        assert(methodMeta != nullptr);

        novanet::example::AddRequest request;
        request.set_lhs(10);
        request.set_rhs(20);

        std::string requestBytes;
        assert(request.SerializeToString(&requestBytes));

        MethodInvoker invoker;

        std::string responseBytes;
        errorText.clear();

        const bool ok = invoker.invokeUnary(
            *serviceMeta, *methodMeta, requestBytes, responseBytes, errorText);

        assert(!ok);
        assert(responseBytes.empty());
        assert(errorText.find("intentional failure") != std::string::npos);
    }

    std::cout << "[PASS] MethodInvokerTest passed.\n";
    return 0;
}