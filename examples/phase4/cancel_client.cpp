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
    const std::string prompt = argOr(argc, argv, 3, "cancel this stream");

    novanet::rpc::RpcClient client(novanet::net::InetAddress(host, port),
                                   "phase4_cancel_client");

    std::string errorText;
    if (!client.connect(&errorText)) {
        std::cerr << "connect failed: " << errorText << "\n";
        return 1;
    }

    const auto request = makeGenerateRequest(prompt, "cancel-demo");

    std::mutex mutex;
    std::condition_variable cv;
    bool firstData = false;
    bool finished = false;
    std::uint32_t streamId = 0;

    novanet::rpc::RpcChannel::StreamCallbacks callbacks;
    callbacks.onData = [&](std::uint32_t id, std::uint64_t sequence,
                           const novanet::ai::chat::GenerateChunk& chunk) {
        printStreamData(id, sequence, chunk);
        {
            std::lock_guard<std::mutex> lock(mutex);
            firstData = true;
            streamId = id;
        }
        cv.notify_one();
    };
    callbacks.onEnd = [&](std::uint32_t id,
                          novanet::rpc::meta::RpcErrorCode code,
                          std::string text) {
        printStreamEnd(id, code, text);
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        cv.notify_one();
    };
    callbacks.onError = [&](std::uint32_t id,
                            novanet::rpc::meta::RpcErrorCode code,
                            std::string text) {
        std::cout << "STREAM_ERROR stream=" << id << " code=" << code
                  << " text=\"" << text << "\"\n";
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        cv.notify_one();
    };

    auto handle = client.openStream("novanet.ai.chat.ChatService", "Generate",
                                    request, std::move(callbacks));
    if (!handle) {
        std::cerr << "open stream failed: " << handle.errorText << "\n";
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(3),
                         [&]() { return firstData || finished; })) {
            std::cerr << "wait first DATA timeout\n";
            return 1;
        }
    }

    const std::uint32_t id = streamId == 0 ? handle.streamId : streamId;
    if (!client.cancelStream(id, "client cancels after first chunk")) {
        std::cerr << "cancel stream failed\n";
        return 1;
    }

    std::cout << "sent STREAM_CANCEL stream=" << id << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.disconnect();
    return 0;
}
