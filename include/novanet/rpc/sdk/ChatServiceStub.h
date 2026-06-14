#pragma once

#include <memory>
#include <string>

#include "chat.pb.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ClientChannel.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/ClientReader.h"
#include "novanet/rpc/sdk/StubBase.h"

namespace novanet::rpc::sdk {

/*
 * ChatServiceStub 是 ChatService 的强类型 SDK Stub。
 *
 * Phase 4 SDK 目标 API：
 *
 *   ChatServiceStub chat(channel);
 *
 *   ClientContext ctx;
 *   GenerateRequest req;
 *
 *   auto reader = chat.Generate(&ctx, req);
 *
 *   GenerateChunk chunk;
 *   while (reader->Read(&chunk)) {
 *       ...
 *   }
 *
 *   RpcStatus status = reader->Finish();
 *
 * 职责：
 * - 提供 Generate() server streaming API；
 * - 创建 ClientReader<GenerateChunk>；
 * - 注册 RpcChannel::StreamCallbacks；
 * - 调用 ClientChannel::openStream()；
 * - 不直接操作 RpcClient / RpcChannel / TcpConnection；
 * - 不处理 RpcCodec / RpcMessage；
 * - 不做服务发现 / 负载均衡 / 重试 / 认证 / 拦截器。
 */
class ChatServiceStub final : public StubBase {
public:
    using GenerateRequest = novanet::ai::chat::GenerateRequest;
    using GenerateChunk = novanet::ai::chat::GenerateChunk;
    using GenerateReader = ClientReader<GenerateChunk>;

    explicit ChatServiceStub(std::shared_ptr<ClientChannel> channel);

    ~ChatServiceStub() override = default;

    ChatServiceStub(const ChatServiceStub&) = default;
    ChatServiceStub& operator=(const ChatServiceStub&) = default;

    ChatServiceStub(ChatServiceStub&&) noexcept = default;
    ChatServiceStub& operator=(ChatServiceStub&&) noexcept = default;

    /*
     * 打开 ChatService.Generate server streaming。
     *
     * 返回：
     * - 永远返回非空 reader；
     * - 如果打开 stream 失败，reader->Read(...) 会返回 false，
     *   reader->Finish() 返回失败 RpcStatus。
     *
     */
    [[nodiscard]] std::unique_ptr<GenerateReader> Generate(
        ClientContext* ctx, const GenerateRequest& request);

    [[nodiscard]] static constexpr const char* serviceName() noexcept {
        return kServiceName;
    }

    [[nodiscard]] static constexpr const char* generateMethodName() noexcept {
        return kGenerateMethodName;
    }

private:
    static void markReaderFailed(GenerateReader* reader,
                                 novanet::rpc::meta::RpcErrorCode errorCode,
                                 const std::string& errorText);

private:
    /*
     *   package novanet.ai.chat;
     *   service ChatService { ... }
     *
     *
     *   novanet.ai.chat.ChatService
     */
    static constexpr const char* kServiceName = "novanet.ai.chat.ChatService";

    static constexpr const char* kGenerateMethodName = "Generate";
};

}  // namespace novanet::rpc::sdk