#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "calculator.pb.h"
#include "chat.pb.h"
#include "novanet/rpc/core/AiProvider.h"
#include "rpc_meta.pb.h"

namespace novanet::examples::phase4 {

constexpr const char* kLocalhost = "127.0.0.1";
constexpr std::uint16_t kCalculatorPort = 19091;
constexpr std::uint16_t kChatPort = 19092;
constexpr std::uint32_t kDemoStreamId = 1;
constexpr std::uint64_t kDemoRequestId = 1;

inline std::uint16_t parsePort(int argc, char** argv, int index,
                               std::uint16_t fallback) {
    if (argc <= index) {
        return fallback;
    }

    const int value = std::stoi(argv[index]);
    if (value <= 0 || value > 65535) {
        throw std::runtime_error("invalid port");
    }

    return static_cast<std::uint16_t>(value);
}

inline std::string argOr(int argc, char** argv, int index, std::string fallback) {
    if (argc <= index) {
        return fallback;
    }

    return argv[index];
}

class CalculatorServiceImpl final
    : public novanet::example::calculator::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::calculator::AddRequest* request,
             ::novanet::example::calculator::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        if (request == nullptr || response == nullptr) {
            if (controller != nullptr) {
                controller->SetFailed("CalculatorService.Add got null request/response");
            }
            return;
        }

        response->set_result(request->lhs() + request->rhs());

        if (done != nullptr) {
            done->Run();
        }
    }
};

class ChatServiceDescriptorImpl final : public novanet::ai::chat::ChatService {
public:
    void Generate(::google::protobuf::RpcController* controller,
                  const ::novanet::ai::chat::GenerateRequest* request,
                  ::novanet::ai::chat::GenerateResponse* response,
                  ::google::protobuf::Closure* done) override {
        (void)request;
        (void)response;

        if (controller != nullptr) {
            controller->SetFailed(
                "ChatService.Generate is served by NovaNet server streaming");
        }

        if (done != nullptr) {
            done->Run();
        }
    }
};

class ExampleAiProvider final : public novanet::rpc::AiProvider {
public:
    Status generateStreaming(const novanet::ai::chat::GenerateRequest& request,
                             ChunkSink onChunk,
                             StopChecker shouldStop) override {
        if (!onChunk) {
            return Status::invalidRequest("ChunkSink is empty");
        }

        const std::string userText = lastUserMessage(request);
        if (userText.empty()) {
            return Status::invalidRequest("GenerateRequest has no user message");
        }

        if (request.model() == "cancel-demo") {
            return emitMany(onChunk, shouldStop, userText, 100, 35,
                            "cancel-demo ");
        }

        if (request.model() == "backpressure-demo") {
            return emitMany(onChunk, shouldStop, userText, 20000, 0,
                            makeLargeDelta());
        }

        Status status = emit(onChunk, shouldStop, 0, "Echo: ");
        if (!status.ok()) {
            return status;
        }

        status = emit(onChunk, shouldStop, 1, userText);
        if (!status.ok()) {
            return status;
        }

        return emit(onChunk, shouldStop, 2, " [phase4-example]", "stop");
    }

private:
    static Status emit(ChunkSink& onChunk, StopChecker& shouldStop,
                       std::uint32_t index, std::string delta,
                       std::string finishReason = "") {
        if (shouldStop) {
            Status stop = shouldStop();
            if (!stop.ok()) {
                return stop;
            }
        }

        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(index);
        chunk.set_delta(std::move(delta));
        chunk.set_finish_reason(std::move(finishReason));

        return onChunk(chunk);
    }

    static Status emitMany(ChunkSink& onChunk,
                           StopChecker& shouldStop,
                           const std::string& userText,
                           std::uint32_t count,
                           int delayMillis,
                           const std::string& prefix) {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::ostringstream out;
            out << prefix << "#" << i << " " << userText;

            std::string finish;
            if (i + 1 == count) {
                finish = "stop";
            }

            Status status = emit(onChunk, shouldStop, i, out.str(), finish);
            if (!status.ok()) {
                return status;
            }

            if (delayMillis > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delayMillis));
            }
        }

        return Status::success();
    }

    static std::string lastUserMessage(
        const novanet::ai::chat::GenerateRequest& request) {
        for (int i = request.messages_size() - 1; i >= 0; --i) {
            const auto& message = request.messages(i);
            if (message.role() == "user" && !message.content().empty()) {
                return message.content();
            }
        }

        if (request.messages_size() > 0) {
            const auto& last = request.messages(request.messages_size() - 1);
            return last.content();
        }

        return {};
    }

    static std::string makeLargeDelta() {
        std::string data;
        data.reserve(4096);
        data.append("backpressure-block ");
        data.resize(4096, 'x');
        return data;
    }
};

inline novanet::ai::chat::GenerateRequest makeGenerateRequest(
    std::string prompt, std::string model) {
    novanet::ai::chat::GenerateRequest request;
    request.set_model(std::move(model));
    request.set_max_tokens(256);
    request.set_temperature(0.2F);

    auto* message = request.add_messages();
    message->set_role("user");
    message->set_content(std::move(prompt));

    return request;
}

inline void printStreamData(std::uint32_t streamId, std::uint64_t sequence,
                            const novanet::ai::chat::GenerateChunk& chunk,
                            bool compact = false) {
    std::string delta = chunk.delta();
    if (compact && delta.size() > 96) {
        delta.resize(96);
        delta.append("...");
    }

    std::cout << "DATA stream=" << streamId
              << " seq=" << sequence
              << " chunk.index=" << chunk.index()
              << " delta=\"" << delta << "\"";

    if (!chunk.finish_reason().empty()) {
        std::cout << " finish_reason=" << chunk.finish_reason();
    }

    std::cout << "\n";
}

inline void printStreamEnd(std::uint32_t streamId,
                           novanet::rpc::meta::RpcErrorCode code,
                           const std::string& text) {
    std::cout << "END stream=" << streamId << " code=" << code
              << " text=\"" << text << "\"\n";
}

}  // namespace novanet::examples::phase4
