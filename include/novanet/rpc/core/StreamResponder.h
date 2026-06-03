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
 * 它用于把 AiProvider worker 线程中生成的 GenerateChunk
 * 安全地交给 RPC 网络层发送。
 *
 * 设计目标：
 * - worker 线程可以调用 sendData / sendEnd / sendError；
 * - 具体实现不能在 worker 线程直接操作 TcpConnection；
 * - 具体实现必须通过 EventLoop::queueInLoop 回到连接所属线程；
 * - 支持 cancel / timeout / backpressure / connection closed；
 * - 配合 AiProvider::ChunkSink / StopChecker 使用。
 */
class StreamResponder {
public:
    virtual ~StreamResponder() = default;

    StreamResponder(const StreamResponder&) = delete;
    StreamResponder& operator=(const StreamResponder&) = delete;

    StreamResponder(StreamResponder&&) = delete;
    StreamResponder& operator=(StreamResponder&&) = delete;

    /*
     * 发送一个 streaming data chunk。
     *
     * 语义：
     *   GenerateChunk
     *      -> StreamDataMeta
     *      -> RpcMessage(STREAM_DATA)
     *      -> queueInLoop
     *      -> TcpConnection::send
     */
    [[nodiscard]] virtual AiProvider::Status sendData(
        std::uint32_t streamId, std::uint64_t requestId,
        const novanet::ai::chat::GenerateChunk& chunk) = 0;

    /*
     * 发送 STREAM_END。
     *
     * 正常结束：
     *   errorCode = RPC_OK
     *
     * 异常结束：
     *   errorCode != RPC_OK
     *
     * 注意：
     *   对 stream 级终止错误，优先使用 sendEnd(errorCode, errorText)，
     *   而不是 sendError()。
     */
    [[nodiscard]] virtual AiProvider::Status sendEnd(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText) = 0;

    /*
     * 发送 ERROR_FRAME。
     *
     * 语义：
     * - ERROR_FRAME 主要用于协议级错误通知；
     * - 它本身不默认终止 stream；
     * - 如果需要终止某个 stream，应使用 sendEnd(errorCode, errorText)。
     */
    [[nodiscard]] virtual AiProvider::Status sendError(
        std::uint32_t streamId, std::uint64_t requestId,
        novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText) = 0;

    /*
     * 供 AiProvider::StopChecker 使用。
     *
     * 如果返回非 OK，Provider 必须停止生成。
     */
    [[nodiscard]] virtual AiProvider::Status shouldStop(
        std::uint32_t streamId) const = 0;

    /*
     * 连接关闭时由 RpcServer 调用。
     *
     * 作用：
     * - 阻止后续 worker 继续发送；
     * - 让 shouldStop 返回 cancelled；
     * - 清理 responder 内部状态。
     */
    virtual void markConnectionClosed() = 0;

protected:
    StreamResponder() = default;
};

}  // namespace novanet::rpc