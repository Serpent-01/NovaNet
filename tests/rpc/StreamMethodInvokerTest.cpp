#include <cassert>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "chat.pb.h"
#include "novanet/rpc/core/AiExecutor.h"
#include "novanet/rpc/core/ChatGenerateStreamHandler.h"
#include "novanet/rpc/core/FakeAiProvider.h"
#include "novanet/rpc/core/ServiceRegistry.h"
#include "novanet/rpc/core/StreamMethodInvoker.h"
#include "novanet/rpc/core/StreamResponder.h"
#include "rpc_meta.pb.h"

namespace {

class ChatServiceImpl final : public novanet::ai::chat::ChatService {
public:
    void Generate(::google::protobuf::RpcController*,
                  const ::novanet::ai::chat::GenerateRequest*,
                  ::novanet::ai::chat::GenerateResponse*,
                  ::google::protobuf::Closure* done) override {
        if (done != nullptr) {
            done->Run();
        }
    }
};

class CapturingResponder final : public novanet::rpc::StreamResponder {
public:
    [[nodiscard]] novanet::rpc::AiProvider::Status sendData(
        std::uint32_t streamId, std::uint64_t requestId,
        const novanet::ai::chat::GenerateChunk& chunk) override {
        std::lock_guard<std::mutex> lock(mutex_);
        streamId_ = streamId;
        requestId_ = requestId;
        deltas_.push_back(chunk.delta());
        return novanet::rpc::AiProvider::Status::success();
    }

    [[nodiscard]] novanet::rpc::AiProvider::Status sendEnd(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode,
        std::string errorText) override {
        std::lock_guard<std::mutex> lock(mutex_);
        streamId_ = streamId;
        requestId_ = requestId;
        endCode_ = errorCode;
        endText_ = std::move(errorText);
        ended_ = true;
        return novanet::rpc::AiProvider::Status::success();
    }

    [[nodiscard]] novanet::rpc::AiProvider::Status sendError(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode,
        std::string errorText) override {
        return sendEnd(streamId, requestId, errorCode, std::move(errorText));
    }

    [[nodiscard]] novanet::rpc::AiProvider::Status shouldStop(
        std::uint32_t) const override {
        return novanet::rpc::AiProvider::Status::success();
    }

    void markConnectionClosed(std::string) override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    [[nodiscard]] std::vector<std::string> deltas() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deltas_;
    }

    [[nodiscard]] bool ended() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ended_;
    }

    [[nodiscard]] novanet::rpc::meta::RpcErrorCode endCode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return endCode_;
    }

    [[nodiscard]] std::uint32_t streamId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return streamId_;
    }

    [[nodiscard]] std::uint64_t requestId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestId_;
    }

private:
    mutable std::mutex mutex_;
    std::uint32_t streamId_{0};
    std::uint64_t requestId_{0};
    std::vector<std::string> deltas_;
    bool ended_{false};
    bool closed_{false};
    novanet::rpc::meta::RpcErrorCode endCode_{novanet::rpc::meta::RPC_UNKNOWN_ERROR};
    std::string endText_;
};

std::string makeGenerateRequestPayload() {
    novanet::ai::chat::GenerateRequest request;
    auto* message = request.add_messages();
    message->set_role("user");
    message->set_content("hello");
    request.set_model("fake-llm");
    request.set_max_tokens(16);
    request.set_temperature(0.0F);

    std::string payload;
    assert(request.SerializeToString(&payload));
    return payload;
}

}  // namespace

int main() {
    novanet::rpc::ServiceRegistry registry;
    ChatServiceImpl chatService;
    std::string errorText;
    assert(registry.registerService(&chatService, &errorText));

    novanet::rpc::StreamMethodInvoker invoker(registry);

    {
        const auto status = invoker.validate(
            novanet::rpc::ChatGenerateStreamHandler::serviceFullName(),
            novanet::rpc::ChatGenerateStreamHandler::generateMethodName());
        assert(status.failed());
        assert(status.errorCode() == novanet::rpc::meta::RPC_METHOD_NOT_FOUND);
    }

    novanet::rpc::FakeAiProvider aiProvider;
    novanet::rpc::AiExecutor executor;
    novanet::rpc::ChatGenerateStreamHandler handler(aiProvider, executor);

    assert(invoker.registerHandler(&handler, &errorText));
    assert(invoker.handlerCount() == 1);

    {
        const auto status = invoker.validate(
            novanet::rpc::ChatGenerateStreamHandler::serviceFullName(),
            novanet::rpc::ChatGenerateStreamHandler::generateMethodName());
        assert(status.ok());
    }

    {
        const auto responder = std::make_shared<CapturingResponder>();
        auto status = invoker.start(
            1, 1001, novanet::rpc::ChatGenerateStreamHandler::serviceFullName(),
            novanet::rpc::ChatGenerateStreamHandler::generateMethodName(),
            "not a protobuf payload", responder);
        assert(status.failed());
        assert(status.errorCode() == novanet::rpc::meta::RPC_PARSE_REQUEST_FAILED);
    }

    assert(executor.start());

    const auto responder = std::make_shared<CapturingResponder>();
    auto status = invoker.start(
        7, 7001, novanet::rpc::ChatGenerateStreamHandler::serviceFullName(),
        novanet::rpc::ChatGenerateStreamHandler::generateMethodName(),
        makeGenerateRequestPayload(), responder);
    assert(status.ok());

    executor.stop(novanet::rpc::AiExecutor::StopMode::kDrain);

    const auto deltas = responder->deltas();
    assert(deltas.size() == 3);
    assert(deltas[0] == "Echo: ");
    assert(deltas[1] == "hello");
    assert(deltas[2] == " [fake-llm]");
    assert(responder->ended());
    assert(responder->endCode() == novanet::rpc::meta::RPC_OK);
    assert(responder->streamId() == 7);
    assert(responder->requestId() == 7001);

    std::cout << "[PASS] StreamMethodInvokerTest passed.\n";
    return 0;
}
