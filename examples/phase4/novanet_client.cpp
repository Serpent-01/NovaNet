#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "Phase4ExampleSupport.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcClient.h"

namespace {

constexpr std::uint16_t kDefaultPort = 19090;
constexpr const char* kCalculatorService =
    "novanet.example.calculator.CalculatorService";
constexpr const char* kChatService = "novanet.ai.chat.ChatService";

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [command] [host] [port] [args...]\n"
        << "Commands:\n"
        << "  all                         run add, heartbeat, chat, cancel, backpressure\n"
        << "  add [lhs] [rhs]             unary CalculatorService.Add\n"
        << "  chat [prompt] [model]       streaming ChatService.Generate\n"
        << "  cancel [prompt]             open stream, then send STREAM_CANCEL\n"
        << "  backpressure [prompt]       slow client, expect RPC_BACKPRESSURE\n"
        << "  heartbeat                   send HEARTBEAT_PING\n"
        << "Defaults: command=all host=127.0.0.1 port=" << kDefaultPort << "\n";
}

int runAdd(novanet::rpc::RpcClient& client, int argc, char** argv,
           int argIndex) {
    const std::int64_t lhs = argc > argIndex ? std::stoll(argv[argIndex]) : 1;
    const std::int64_t rhs =
        argc > argIndex + 1 ? std::stoll(argv[argIndex + 1]) : 2;

    novanet::example::calculator::AddRequest request;
    request.set_lhs(lhs);
    request.set_rhs(rhs);

    novanet::example::calculator::AddResponse response;
    const auto status = client.callUnary(kCalculatorService, "Add", request,
                                         &response, std::chrono::seconds(3));

    if (status.failed()) {
        std::cerr << "ADD failed: " << status.toString() << "\n";
        return 1;
    }

    std::cout << "ADD " << lhs << " + " << rhs << " = "
              << response.result() << "\n";
    return 0;
}

int runHeartbeat(novanet::rpc::RpcClient& client) {
    if (!client.sendHeartbeatPing()) {
        std::cerr << "HEARTBEAT_PING failed\n";
        return 1;
    }

    std::cout << "HEARTBEAT_PING sent\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

int runStream(novanet::rpc::RpcClient& client, std::string prompt,
              std::string model, bool cancelAfterFirstData,
              bool expectBackpressure, std::chrono::seconds waitTimeout) {
    using novanet::rpc::meta::RPC_BACKPRESSURE;
    using novanet::rpc::meta::RPC_OK;

    const auto request = novanet::examples::phase4::makeGenerateRequest(
        std::move(prompt), std::move(model));

    std::mutex mutex;
    std::condition_variable cv;
    bool firstData = false;
    bool finished = false;
    int exitCode = 0;
    std::uint32_t dataFrames = 0;
    std::uint32_t observedStreamId = 0;

    novanet::rpc::RpcChannel::StreamCallbacks callbacks;
    callbacks.onData = [&](std::uint32_t streamId, std::uint64_t sequence,
                           const novanet::ai::chat::GenerateChunk& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            firstData = true;
            observedStreamId = streamId;
            ++dataFrames;

            if (!expectBackpressure || dataFrames <= 8 ||
                dataFrames % 100 == 0) {
                novanet::examples::phase4::printStreamData(
                    streamId, sequence, chunk, expectBackpressure);
            }
        }

        cv.notify_one();

        if (expectBackpressure) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    callbacks.onEnd = [&](std::uint32_t streamId,
                          novanet::rpc::meta::RpcErrorCode code,
                          std::string text) {
        novanet::examples::phase4::printStreamEnd(streamId, code, text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
            if (expectBackpressure) {
                exitCode = code == RPC_BACKPRESSURE ? 0 : 1;
            } else {
                exitCode = code == RPC_OK ? 0 : 1;
            }
        }
        cv.notify_one();
    };

    callbacks.onError = [&](std::uint32_t streamId,
                            novanet::rpc::meta::RpcErrorCode code,
                            std::string text) {
        std::cout << "STREAM_ERROR stream=" << streamId << " code=" << code
                  << " text=\"" << text << "\"\n";
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
            exitCode = expectBackpressure && code == RPC_BACKPRESSURE ? 0 : 1;
        }
        cv.notify_one();
    };

    auto handle = client.openStream(kChatService, "Generate", request,
                                    std::move(callbacks));
    if (!handle) {
        std::cerr << "STREAM_OPEN failed: " << handle.errorText << "\n";
        return 1;
    }

    if (cancelAfterFirstData) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(3),
                             [&]() { return firstData || finished; })) {
                std::cerr << "wait first DATA timeout\n";
                return 1;
            }
        }

        const std::uint32_t streamId =
            observedStreamId == 0 ? handle.streamId : observedStreamId;
        if (!client.cancelStream(streamId, "client cancels after first chunk")) {
            std::cerr << "STREAM_CANCEL failed\n";
            return 1;
        }

        std::cout << "STREAM_CANCEL sent stream=" << streamId << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }

    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, waitTimeout, [&]() { return finished; })) {
        std::cerr << "stream wait timeout after " << dataFrames
                  << " DATA frames\n";
        return 1;
    }

    return exitCode;
}

int runCommand(novanet::rpc::RpcClient& client, const std::string& command,
               int argc, char** argv, int argIndex) {
    if (command == "add") {
        return runAdd(client, argc, argv, argIndex);
    }

    if (command == "heartbeat") {
        return runHeartbeat(client);
    }

    if (command == "chat") {
        const std::string prompt =
            argc > argIndex ? argv[argIndex] : "hello unified novanet";
        const std::string model =
            argc > argIndex + 1 ? argv[argIndex + 1] : "demo-chat-model";
        return runStream(client, prompt, model, false, false,
                         std::chrono::seconds(5));
    }

    if (command == "cancel") {
        const std::string prompt =
            argc > argIndex ? argv[argIndex] : "cancel this stream";
        return runStream(client, prompt, "cancel-demo", true, false,
                         std::chrono::seconds(3));
    }

    if (command == "backpressure") {
        const std::string prompt =
            argc > argIndex ? argv[argIndex] : "slow client";
        return runStream(client, prompt, "backpressure-demo", false, true,
                         std::chrono::seconds(15));
    }

    if (command == "all") {
        if (runAdd(client, argc, argv, argIndex) != 0) {
            return 1;
        }
        if (runHeartbeat(client) != 0) {
            return 1;
        }
        if (runStream(client, "hello unified novanet", "demo-chat-model", false,
                      false, std::chrono::seconds(5)) != 0) {
            return 1;
        }
        if (runStream(client, "cancel this stream", "cancel-demo", true, false,
                      std::chrono::seconds(3)) != 0) {
            return 1;
        }
        return runStream(client, "slow client", "backpressure-demo", false,
                         true, std::chrono::seconds(15));
    }

    std::cerr << "unknown command: " << command << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    if (argc > 1 && (std::string(argv[1]) == "-h" ||
                     std::string(argv[1]) == "--help")) {
        printUsage(argv[0]);
        return 0;
    }

    try {
        const std::string command = argOr(argc, argv, 1, "all");
        const std::string host = argOr(argc, argv, 2, kLocalhost);
        const std::uint16_t port = parsePort(argc, argv, 3, kDefaultPort);
        constexpr int kFirstCommandArg = 4;

        novanet::rpc::RpcClient client(novanet::net::InetAddress(host, port),
                                       "novanet_client");

        std::string errorText;
        if (!client.connect(&errorText)) {
            std::cerr << "connect failed: " << errorText << "\n";
            return 1;
        }

        return runCommand(client, command, argc, argv, kFirstCommandArg);
    } catch (const std::exception& ex) {
        std::cerr << "novanet_client failed: " << ex.what() << "\n";
        return 1;
    }
}
