#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#include "Phase4ExampleSupport.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcClient.h"

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    const std::string host = argOr(argc, argv, 1, kLocalhost);
    const std::uint16_t port = parsePort(argc, argv, 2, kChatPort);
    const std::string prompt = argOr(argc, argv, 3, "hello phase4");

    novanet::rpc::RpcClient client(novanet::net::InetAddress(host, port),
                                   "phase4_chat_client");

    std::string errorText;
    if (!client.connect(&errorText)) {
        std::cerr << "connect failed: " << errorText << "\n";
        return 1;
    }

    if (!client.sendHeartbeatPing()) {
        std::cerr << "send HEARTBEAT_PING failed\n";
        return 1;
    }
    std::cout << "HEARTBEAT_PING sent\n";

    const auto request = makeGenerateRequest(prompt, "demo-chat-model");

    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    int exitCode = 0;

    novanet::rpc::RpcChannel::StreamCallbacks callbacks;
    callbacks.onData = [](std::uint32_t streamId, std::uint64_t sequence,
                          const novanet::ai::chat::GenerateChunk& chunk) {
        printStreamData(streamId, sequence, chunk);
    };
    callbacks.onEnd = [&](std::uint32_t streamId,
                          novanet::rpc::meta::RpcErrorCode code,
                          std::string text) {
        printStreamEnd(streamId, code, text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        cv.notify_one();
    };
    callbacks.onError = [&](std::uint32_t streamId,
                            novanet::rpc::meta::RpcErrorCode code,
                            std::string text) {
        std::cerr << "STREAM_ERROR stream=" << streamId << " code=" << code
                  << " text=\"" << text << "\"\n";
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
            exitCode = 1;
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
    if (!cv.wait_for(lock, std::chrono::seconds(5), [&]() { return finished; })) {
        std::cerr << "stream wait timeout\n";
        return 1;
    }

    client.disconnect();
    return exitCode;
}
