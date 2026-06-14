#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "Phase4ExampleSupport.h"
#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/GatewayAiProvider.h"
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

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

long envLongOr(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    try {
        return std::stol(value);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    if (argc > 1 &&
        (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
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

        /*
         * GatewayAiProvider 通过 Python AI Bridge 调真实 AI。
         *
         * 默认调用：
         *   http://127.0.0.1:18080/chat/stream
         *
         * 可以通过环境变量覆盖：
         *   NOVANET_AI_BRIDGE_ENDPOINT
         *   NOVANET_AI_MODEL
         */
        novanet::rpc::GatewayAiProvider::Options aiOptions;
        aiOptions.endpoint = envOr("NOVANET_AI_BRIDGE_ENDPOINT",
                                   "http://127.0.0.1:18080/chat/stream");
        aiOptions.model = envOr("NOVANET_AI_MODEL", "deepseek-chat");
        aiOptions.connectTimeoutMs =
            envLongOr("NOVANET_AI_CONNECT_TIMEOUT_MS", 3000);

        /*
         * streaming 请求不建议设置很短的总超时。
         * 0 表示不设置 libcurl 总超时。
         */
        aiOptions.totalTimeoutMs = envLongOr("NOVANET_AI_TOTAL_TIMEOUT_MS", 0);

        aiOptions.lowSpeedTimeSeconds =
            envLongOr("NOVANET_AI_LOW_SPEED_TIME_SECONDS", 60);
        aiOptions.lowSpeedLimitBytesPerSecond =
            envLongOr("NOVANET_AI_LOW_SPEED_LIMIT_BPS", 1);

        novanet::rpc::GatewayAiProvider aiProvider(aiOptions);

        novanet::rpc::RpcServer::Options options;
        options.aiExecutorOptions.workerCount = 2;
        options.aiExecutorOptions.maxQueueSize = 128;

        /*
         * 真实 AI 输出速度通常比 FakeAiProvider 慢很多。
         * highWaterMark 可以先保持 32KB，方便你继续验证 backpressure。
         */
        options.streamResponderOptions.highWaterMarkBytes = 32 * 1024;
        options.streamResponderOptions.maxPendingDataMessages = 128;

        options.streamTimeoutScanIntervalSeconds = 1.0;

        /*
         * 真实 AI 首 token 和完整输出可能比 fake 慢。
         * 30 秒有时偏短，可以先调到 120 秒。
         */
        options.streamIdleTimeoutSeconds = 120.0;

        novanet::rpc::RpcServer server(&loop, listenAddr, "novanet_server",
                                       aiProvider, options);
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
        std::cout << "AI bridge endpoint: " << aiOptions.endpoint << "\n";
        std::cout << "AI model: " << aiOptions.model << "\n";
        std::cout << "registered services:\n"
                  << "  novanet.example.calculator.CalculatorService.Add\n"
                  << "  novanet.ai.chat.ChatService.Generate\n";
        std::cout << "client commands: add, chat, cancel, heartbeat\n";
        std::cout << "warning: backpressure/all may consume real AI tokens\n";

        loop.loop();
        server.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "novanet_server failed: " << ex.what() << "\n";
        return 1;
    }
}