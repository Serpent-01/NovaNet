#include <google/protobuf/message.h>
#include <google/protobuf/stubs/callback.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "calculator.pb.h"

class CalculatorServiceImpl final : public novanet::example::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::AddRequest* request,
             ::novanet::example::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        (void)controller;

        response->set_result(request->lhs() + request->rhs());
        if (done != nullptr) {
            done->Run();
        }
    }
};

int main() {
    using namespace novanet::example;

    AddRequest req;

    req.set_lhs(1);
    req.set_rhs(2);

    std::string reqBytes;
    // SerializeToString 将对象 ---> 字节流
    assert(req.SerializeToString(&reqBytes));
    assert(!reqBytes.empty());

    AddRequest parsedReq;
    //反序列化：字节流 -> 对象
    assert(parsedReq.ParseFromString(reqBytes));
    assert(parsedReq.lhs() == 1);
    assert(parsedReq.rhs() == 2);

    // 2. 测 AddResponse 序列化 / 反序列化
    AddResponse resp;
    resp.set_result(3);

    std::string respBytes;
    assert(resp.SerializeToString(&respBytes));
    assert(!respBytes.empty());

    AddResponse parsedResp;
    assert(parsedResp.ParseFromString(respBytes));
    assert(parsedResp.result() == 3);

    // 3. 测 service descriptor
    CalculatorServiceImpl service;
    const auto* descriptor = service.GetDescriptor();
    assert(descriptor != nullptr);
    assert(descriptor->name() == "CalculatorService");
    assert(descriptor->method_count() == 1);

    const auto* method = descriptor->FindMethodByName("Add");
    assert(method != nullptr);
    assert(method->name() == "Add");
    assert(method->input_type()->name() == "AddRequest");
    assert(method->output_type()->name() == "AddResponse");

    // 4. 直接通过 protobuf Service 接口调用 Add
    std::unique_ptr<::google::protobuf::Message> requset(
        service.GetRequestPrototype(method).New());

    std::unique_ptr<::google::protobuf::Message> response(
        service.GetResponsePrototype(method).New());

    auto* addReq = dynamic_cast<AddRequest*>(requset.get());
    auto* addResp = dynamic_cast<AddResponse*>(response.get());

    assert(addReq != nullptr);
    assert(addResp != nullptr);
    addReq->set_lhs(1);
    addReq->set_rhs(2);

    service.CallMethod(method, nullptr, addReq, addResp, nullptr);
    assert(addResp->result() == 3);

    std::cout << "[PASS] CalculatorProtoSmokeTest passed.\n";
    return 0;
}
