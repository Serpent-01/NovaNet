#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "Phase4ExampleSupport.h"
#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcServer.h"

namespace {

constexpr std::uint16_t kDefaultPort = 19090;

std::atomic<bool> g_stop{false};

void handleSignal(int) {
    g_stop.store(true, std::memory_order_release);
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [port]\n"
              << "Default port: " << kDefaultPort << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        printUsage(argv[0]);
        return 0;
    }

    try {
        novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        const std::uint16_t port = parsePort(argc, argv, 1, kDefaultPort);

        novanet::net::EventLoop loop;
        novanet::net::InetAddress listenAddr(port, true);

        ExampleAiProvider aiProvider;

        novanet::rpc::RpcServer::Options options;
        options.aiExecutorOptions.workerCount = 2;
        options.aiExecutorOptions.maxQueueSize = 128;
        options.streamResponderOptions.highWaterMarkBytes = 32 * 1024;
        options.streamResponderOptions.maxPendingDataMessages = 128;
        options.streamTimeoutScanIntervalSeconds = 1.0;
        options.streamIdleTimeoutSeconds = 30.0;

        novanet::rpc::RpcServer server(&loop, listenAddr, "novanet_server", aiProvider,
                                       options);
        server.setThreadNum(2);

        CalculatorServiceImpl calculatorService;
        ChatServiceDescriptorImpl chatService;

        std::string errorText;
        if (!server.registerService(&calculatorService, &errorText)) {
            std::cerr << "register CalculatorService failed: " << errorText << "\n";
            return 1;
        }

        if (!server.registerService(&chatService, &errorText)) {
            std::cerr << "register ChatService failed: " << errorText << "\n";
            return 1;
        }

        if (!server.start()) {
            std::cerr << "RpcServer start failed\n";
            return 1;
        }

        loop.runEvery(0.5, [&loop]() {
            if (g_stop.load(std::memory_order_acquire)) {
                loop.quit();
            }
        });

        std::cout << "novanet_server listening on 127.0.0.1:" << port << "\n";
        std::cout << "registered services:\n"
                  << "  novanet.example.calculator.CalculatorService.Add\n"
                  << "  novanet.ai.chat.ChatService.Generate\n";
        std::cout << "client commands: add, chat, cancel, backpressure, heartbeat, all\n";

        loop.loop();
        server.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "novanet_server failed: " << ex.what() << "\n";
        return 1;
    }
}
