#include <google/protobuf/service.h>
#include <google/protobuf/stubs/common.h>

#include <iostream>
#include <memory>
#include <string>

#include "calculator.pb.h"
#include "chat.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/FakeAiProvider.h"
#include "novanet/rpc/core/RpcServer.h"

namespace calculator = novanet::example::calculator;
namespace chat = novanet::ai::chat;

/*
 * CalculatorServiceImpl 是普通 unary protobuf service。
 *
 * 客户端调用：
 *
 *   CalculatorServiceStub::Add(...)
 *
 * 最终会走：
 *
 *   UNARY_REQUEST
 *      -> RpcDispatcher
 *      -> MethodInvoker
 *      -> CalculatorServiceImpl::Add
 *      -> UNARY_RESPONSE
 */
class CalculatorServiceImpl final : public calculator::CalculatorService {
public:
    void Add(google::protobuf::RpcController* controller,
             const calculator::AddRequest* request,
             calculator::AddResponse* response,
             google::protobuf::Closure* done) override {
        if (request == nullptr || response == nullptr) {
            if (controller != nullptr) {
                controller->SetFailed("Add request or response is null");
            }

            if (done != nullptr) {
                done->Run();
            }

            return;
        }

        response->set_result(request->lhs() + request->rhs());

        if (done != nullptr) {
            done->Run();
        }
    }
};

/*
 * ChatServiceImpl 当前主要用于注册 protobuf service/method。
 *
 * 注意：
 * ChatService.Generate 的 NovaNet server streaming 主路径不是这个 unary 方法。
 *
 * 真正 streaming 路径是：
 *
 *   STREAM_OPEN
 *      -> RpcDispatcher
 *      -> AiExecutor
 *      -> FakeAiProvider / AiProvider
 *      -> StreamResponder::sendData
 *      -> STREAM_DATA*
 *      -> STREAM_END
 *
 * 但是 RpcDispatcher 在处理 STREAM_OPEN 时通常需要校验：
 *
 *   service_name = "novanet.ai.chat.ChatService"
 *   method_name  = "Generate"
 *
 * 所以服务端仍然注册 ChatServiceImpl，让 ServiceRegistry 能找到这个 service/method。
 */
class ChatServiceImpl final : public chat::ChatService {
public:
    void Generate(google::protobuf::RpcController* controller,
                  const chat::GenerateRequest* request,
                  chat::GenerateResponse* response,
                  google::protobuf::Closure* done) override {
        (void)request;

        if (response != nullptr) {
            response->set_full_text(
                "ChatService.Generate should be called through NovaNet "
                "server streaming SDK.");
            response->set_finish_reason("not_streaming_path");
        }

        if (controller != nullptr) {
            controller->SetFailed(
                "ChatService.Generate is a NovaNet streaming method. "
                "Use ChatServiceStub::Generate + ClientReader on client side.");
        }

        if (done != nullptr) {
            done->Run();
        }
    }
};

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    int port = 19090;
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    LOG_INFO << "NovaNet server starting, port=" << port;

    novanet::net::EventLoop loop;

    /*
     * 根据你的 InetAddress 实现选择构造方式。
     *
     * 如果你支持 InetAddress(uint16_t port)，用下面这行。
     * 如果你只支持 InetAddress(string ip, uint16_t port)，改成：
     *
     *   novanet::net::InetAddress listenAddr("0.0.0.0", port);
     */
    novanet::net::InetAddress listenAddr(static_cast<uint16_t>(port));

    /*
     * 当前 Phase 4 不改服务端 gRPC 风格封装。
     * Streaming 业务仍由 AiProvider 提供。
     */
    novanet::rpc::FakeAiProvider aiProvider;

    novanet::rpc::RpcServer::Options options;

    options.aiExecutorOptions.workerCount = 4;
    options.aiExecutorOptions.maxQueueSize = 1024;

    options.streamIdleTimeoutSeconds = 60.0;
    options.streamTimeoutScanIntervalSeconds = 5.0;

    options.streamResponderOptions.highWaterMarkBytes = 8 * 1024 * 1024;
    options.streamResponderOptions.maxPendingDataMessages = 1024;

    novanet::rpc::RpcServer server(&loop, listenAddr, "NovaNetRpcServer", aiProvider,
                                   options);

    /*
     * 普通 unary service。
     */
    CalculatorServiceImpl calculatorService;

    /*
     * 用于注册 ChatService.Generate。
     * 真正 streaming 由 RpcDispatcher + AiProvider 执行。
     */
    ChatServiceImpl chatService;

    if (!server.registerService(&calculatorService)) {
        LOG_FATAL << "register CalculatorService failed";
    }

    if (!server.registerService(&chatService)) {
        LOG_FATAL << "register ChatService failed";
    }

    /*
     * one loop per thread:
     * - main loop 负责 accept；
     * - sub loops 负责连接读写。
     */
    server.setThreadNum(4);

    if (!server.start()) {
        LOG_FATAL << "RpcServer start failed";
    }

    LOG_INFO << "NovaNet RpcServer listening on port=" << port;

    loop.loop();

    google::protobuf::ShutdownProtobufLibrary();

    return 0;
}