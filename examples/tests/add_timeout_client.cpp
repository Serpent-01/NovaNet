#include <google/protobuf/stubs/common.h>

#include <iostream>
#include <memory>
#include <string>

#include "calculator.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/sdk/CalculatorServiceStub.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/CreateChannel.h"

namespace calculator = novanet::example::calculator;

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    std::string target = "127.0.0.1:19090";
    if (argc >= 2) {
        target = argv[1];
    }

    novanet::rpc::sdk::ChannelOptions options;
    options.connectTimeoutSeconds = 1.0;
    options.defaultRpcTimeoutSeconds = 1.0;

    options.startHeartbeat = false;
    options.nodeId = "novanet-add-timeout-test";

    std::string createError;
    auto channel = novanet::rpc::sdk::CreateChannel(target, options, &createError);

    if (!channel) {
        std::cerr << "CreateChannel failed: " << createError << "\n";
        return 1;
    }

    auto connectStatus = channel->connect();
    if (!connectStatus.ok()) {
        std::cerr << "connect failed: " << connectStatus.toString() << "\n";
        return 1;
    }

    novanet::rpc::sdk::CalculatorServiceStub calculatorStub(channel);

    novanet::rpc::sdk::ClientContext addCtx;
    addCtx.setTimeoutSeconds(0.1);
    addCtx.setMetadata("trace_id", "add-timeout-test-001");
    addCtx.setMetadata("client", "novanet-add-timeout-test");

    calculator::AddRequest addReq;
    calculator::AddResponse addResp;

    addReq.set_lhs(1);
    addReq.set_rhs(2);

    auto addStatus = calculatorStub.Add(&addCtx, addReq, &addResp);

    if (addStatus.ok()) {
        std::cerr << "Add timeout test failed: Add unexpectedly succeeded, result="
                  << addResp.result() << "\n";
        channel->shutdown();
        return 1;
    }

    std::string errorText;
    if (addCtx.failed()) {
        errorText = addCtx.errorText();
    } else {
        errorText = addStatus.toString();
    }

    std::cout << "Add failed as expected: " << addStatus.toString() << "\n";

    if (errorText.find("timeout") == std::string::npos &&
        addStatus.toString().find("timeout") == std::string::npos) {
        std::cerr << "Add timeout test failed: error is not timeout, error="
                  << errorText << "\n";
        channel->shutdown();
        return 1;
    }

    std::cout << "Add timeout test passed\n";

    channel->shutdown();

    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}