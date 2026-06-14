#include <atomic>
#include <csignal>
#include <iostream>

#include "Phase4ExampleSupport.h"
#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcServer.h"

namespace {

std::atomic<bool> g_stop{false};

void handleSignal(int) {
    g_stop.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const std::uint16_t port = parsePort(argc, argv, 1, kCalculatorPort);

    novanet::net::EventLoop loop;
    novanet::net::InetAddress listenAddr(port, true);

    ExampleAiProvider aiProvider;
    novanet::rpc::RpcServer server(&loop, listenAddr, "phase4_calculator_server",
                                   aiProvider);

    CalculatorServiceImpl calculator;
    std::string errorText;
    if (!server.registerService(&calculator, &errorText)) {
        std::cerr << "register CalculatorService failed: " << errorText << "\n";
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

    std::cout << "calculator_server listening on 127.0.0.1:" << port << "\n";
    loop.loop();
    server.stop();
    return 0;
}
