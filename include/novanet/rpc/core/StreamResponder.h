#pragma once

#include <cstdint>
#include <string>

#include "chat.pb.h"
#include "novanet/rpc/core/AiProvider.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc {

/*
 * StreamResponder 是 server streaming 的异步响应抽象。
 *
 * 它是 AiExecutor worker 线程和 TcpConnection 所属 EventLoop 之间的桥梁。
 *
 * 要求：
 * - worker 线程可以调用 sendData / sendEnd / sendError；
 * - 实现类不能在 worker 线程直接操作 TcpConnection；
 * - 必须通过 EventLoop::queueInLoop 回到连接所属线程；
 * - shouldStop 必须反映 cancel / timeout / connection closed / backpressure。
 */
class StreamResponder {
public:
    virtual ~StreamResponder() = default;

    StreamResponder(const StreamResponder&) = delete;
    StreamResponder& operator=(const StreamResponder&) = delete;

    StreamResponder(StreamResponder&&) = delete;
    StreamResponder& operator=(StreamResponder&&) = delete;

    [[nodiscard]] virtual AiProvider::Status sendData(
        std::uint32_t streamId, std::uint64_t requestId,
        const novanet::ai::chat::GenerateChunk& chunk) = 0;

    [[nodiscard]] virtual AiProvider::Status sendEnd(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText) = 0;

    [[nodiscard]] virtual AiProvider::Status sendError(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText) = 0;

    [[nodiscard]] virtual AiProvider::Status shouldStop(std::uint32_t streamId) const = 0;

    virtual void markConnectionClosed(std::string reason) = 0;

protected:
    StreamResponder() = default;
};

}  // namespace novanet::rpc