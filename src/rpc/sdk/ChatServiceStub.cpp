#include "novanet/rpc/sdk/ChatServiceStub.h"

#include <utility>

#include "novanet/base/Logger.h"

namespace novanet::rpc::sdk {

namespace meta = novanet::rpc::meta;

ChatServiceStub::ChatServiceStub(std::shared_ptr<ClientChannel> channel)
    : StubBase(std::move(channel)) {
}

std::unique_ptr<ChatServiceStub::GenerateReader> ChatServiceStub::Generate(
    ClientContext* ctx, const GenerateRequest& request) {
    /*
     * 先拷贝一份 shared_ptr<ClientChannel>。
     * 即使 channel 为空，也可以构造 reader，
     * 让用户通过 Finish() 拿到错误。
     */
    std::shared_ptr<ClientChannel> ch = channel();

    auto reader = std::make_unique<GenerateReader>(ch, ctx);

    if (!ch) {
        const std::string error = "ChatServiceStub has null ClientChannel";
        markReaderFailed(reader.get(), meta::RPC_BAD_REQUEST, error);

        LOG_ERROR << "[ChatServiceStub] " << error;

        return reader;
    }

    if (ctx != nullptr && ctx->cancelled()) {
        const std::string reason =
            ctx->cancelReason().empty() ? "client cancelled" : ctx->cancelReason();

        markReaderFailed(reader.get(), meta::RPC_CANCELLED, reason);

        LOG_WARN << "[ChatServiceStub] Generate cancelled before open, reason="
                 << reason;

        return reader;
    }

    /*
     * 确保底层连接可用。
     *
     * ClientChannel::openStream() 内部也会 ensureConnected()，
     * 这里提前检查可以让错误更清晰。
     */
    auto readyStatus = ensureChannelReady();
    if (!readyStatus.ok()) {
        markReaderFailed(reader.get(), readyStatus.errorCode(),
                         readyStatus.errorText());

        LOG_ERROR << "[ChatServiceStub] channel not ready: "
                  << readyStatus.toString();

        return reader;
    }

    auto callbacks = reader->makeCallbacks();

    auto handle = ch->openStream(kServiceName, kGenerateMethodName, request, ctx,
                                 std::move(callbacks));

    if (!handle.ok) {
        const std::string error = handle.errorText.empty()
                                      ? "open ChatService.Generate stream failed"
                                      : handle.errorText;

        /*
         * StreamHandle 目前只带 errorText，不带 errorCode。
         * 这里用 RPC_UNKNOWN_ERROR。
         * 如果你后续把 StreamHandle 扩展 errorCode，
         * 可以改成 handle.errorCode。
         */
        markReaderFailed(reader.get(), meta::RPC_UNKNOWN_ERROR, error);

        LOG_ERROR << "[ChatServiceStub] Generate openStream failed: " << error;

        return reader;
    }

    reader->bindStream(handle);

    LOG_INFO << "[ChatServiceStub] Generate stream opened, streamId="
             << handle.streamId << ", requestId=" << handle.requestId;

    return reader;
}

void ChatServiceStub::markReaderFailed(GenerateReader* reader,
                                       meta::RpcErrorCode errorCode,
                                       const std::string& errorText) {
    if (reader == nullptr) {
        return;
    }

    reader->markStartFailed(errorCode, errorText.empty()
                                           ? "ChatServiceStub Generate failed"
                                           : errorText);
}

}  // namespace novanet::rpc::sdk