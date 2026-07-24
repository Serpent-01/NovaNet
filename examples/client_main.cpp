#include <google/protobuf/stubs/common.h>

#include <iostream>
#include <memory>
#include <string>

#include "calculator.pb.h"
#include "chat.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/CalculatorServiceStub.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ChatServiceStub.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/CreateChannel.h"

namespace calculator = novanet::example::calculator;
namespace chat = novanet::ai::chat;

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    std::string target = "127.0.0.1:19090";
    if (argc >= 2) {
        target = argv[1];
    }

    novanet::rpc::sdk::ChannelOptions options;

    options.connectTimeoutSeconds = 3.0;
    options.defaultRpcTimeoutSeconds = 5.0;

    options.startHeartbeat = true;
    options.heartbeatIntervalSeconds = 10.0;
    options.heartbeatCheckIntervalSeconds = 5.0;
    options.heartbeatTimeoutSeconds = 30.0;

    options.streamIdleTimeoutSeconds = 60.0;
    options.streamTimeoutScanIntervalSeconds = 5.0;

    options.sendHighWaterMarkBytes = 8 * 1024 * 1024;
    options.nodeId = "novanet-client-demo";

    std::string createError;
    auto channel = novanet::rpc::sdk::CreateChannel(target, options, &createError);

    if (!channel) {
        std::cerr << "CreateChannel failed: " << createError << "\n";
        return 1;
    }

    /*
     * 可以不显式 connect。
     * Stub 调用时 ClientChannel 内部会 ensureConnected()。
     *
     * Demo 里显式 connect，方便排查连接错误。
     */
    auto connectStatus = channel->connect();
    if (!connectStatus.ok()) {
        std::cerr << "connect failed: " << connectStatus.toString() << "\n";
        return 1;
    }

    std::cout << "Connected to " << target << "\n";

    // ============================================================
    // 1. Unary RPC: Calculator.Add
    // ============================================================

    novanet::rpc::sdk::CalculatorServiceStub calculatorStub(channel);

    novanet::rpc::sdk::ClientContext addCtx;
    addCtx.setTimeoutSeconds(3.0);
    addCtx.setMetadata("trace_id", "calculator-add-demo-001");
    addCtx.setMetadata("client", "novanet-client-demo");

    calculator::AddRequest addReq;
    calculator::AddResponse addResp;

    addReq.set_lhs(1);
    addReq.set_rhs(2);

    auto addStatus = calculatorStub.Add(&addCtx, addReq, &addResp);

    if (!addStatus.ok()) {
        std::cerr << "Calculator.Add failed: " << addStatus.toString() << "\n";

        if (addCtx.failed()) {
            std::cerr << "ClientContext error: " << addCtx.errorText() << "\n";
        }

        channel->shutdown();
        return 1;
    }

    std::cout << "Calculator.Add result: " << addReq.lhs() << " + " << addReq.rhs()
              << " = " << addResp.result() << "\n";

    // ============================================================
    // 2. Server Streaming RPC: Chat.Generate
    // ============================================================

    novanet::rpc::sdk::ChatServiceStub chatStub(channel);

    novanet::rpc::sdk::ClientContext chatCtx;
    chatCtx.setMetadata("trace_id", "chat-generate-demo-001");
    chatCtx.setMetadata("client", "novanet-client-demo");

    chat::GenerateRequest genReq;
    genReq.set_model("fake-llm");
    genReq.set_max_tokens(64);
    genReq.set_temperature(0.7f);

    auto* userMessage = genReq.add_messages();
    userMessage->set_role("user");
    userMessage->set_content("什么是Reactor?");

    auto reader = chatStub.Generate(&chatCtx, genReq);

    chat::GenerateChunk chunk;

    std::cout << "Chat.Generate streaming output:\n";

    while (reader->Read(&chunk)) {
        std::cout << chunk.delta() << std::flush;

        if (!chunk.finish_reason().empty()) {
            std::cout << "\nfinish_reason: " << chunk.finish_reason() << "\n";
        }
    }

    auto streamStatus = reader->Finish();

    std::cout << "\n";

    if (!streamStatus.ok()) {
        std::cerr << "Chat.Generate failed: " << streamStatus.toString() << "\n";

        if (chatCtx.failed()) {
            std::cerr << "ClientContext error: " << chatCtx.errorText() << "\n";
        }

        channel->shutdown();
        return 1;
    }

    std::cout << "Chat.Generate finished successfully\n";

    channel->shutdown();

    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}