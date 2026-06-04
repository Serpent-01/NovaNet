#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "Phase4ExampleSupport.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcClient.h"

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    const std::string host = argOr(argc, argv, 1, kLocalhost);
    const std::uint16_t port = parsePort(argc, argv, 2, kChatPort);
    const std::string prompt = argOr(argc, argv, 3, "slow client");

    novanet::rpc::RpcClient client(novanet::net::InetAddress(host, port),
                                   "phase4_backpressure_client");

    std::string errorText;
    if (!client.connect(&errorText)) {
        std::cerr << "connect failed: " << errorText << "\n";
        return 1;
    }

    const auto request = makeGenerateRequest(prompt, "backpressure-demo");

    std::cout << "STREAM_OPEN sent; stream callback sleeps to act as a slow "
                 "client...\n";

    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    int exitCode = 0;
    std::uint32_t dataFrames = 0;

    novanet::rpc::RpcChannel::StreamCallbacks callbacks;
    callbacks.onData = [&](std::uint32_t streamId, std::uint64_t sequence,
                           const novanet::ai::chat::GenerateChunk& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++dataFrames;
            if (dataFrames <= 8 || dataFrames % 100 == 0) {
                printStreamData(streamId, sequence, chunk, true);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    callbacks.onEnd = [&](std::uint32_t streamId,
                          novanet::rpc::meta::RpcErrorCode code,
                          std::string text) {
        printStreamEnd(streamId, code, text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
            std::cout << "received " << dataFrames
                      << " DATA frames before stream end\n";
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
            exitCode = code == novanet::rpc::meta::RPC_BACKPRESSURE ? 0 : 1;
        }
        cv.notify_one();
    };

    auto handle = client.openStream("novanet.ai.chat.ChatService", "Generate",
                                    request, std::move(callbacks));
    if (!handle) {
        std::cerr << "open stream failed: " << handle.errorText << "\n";
        return 1;
    }

    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(15), [&]() { return finished; })) {
        std::cerr << "stream wait timeout after " << dataFrames
                  << " DATA frames\n";
        return 1;
    }

    client.disconnect();
    return exitCode;
}
