#include "novanet/rpc/stream/StreamFrame.h"

#include <utility>

#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::rpc {

StreamFrame::StreamFrame(RpcMessage message) : message_(message) {
}

StreamFrame::StreamFrame(FrameType type, std::uint32_t streamId,
                         std::uint64_t requestId, std::string payload)
    : message_(type, streamId, requestId, std::move(payload)) {
}

bool StreamFrame::valid() const noexcept {
    if (!message_.valid()) {
        return false;
    }

    if (!validStreamType(message_.frameType())) {
        return false;
    }
    /*
     * stream frame 必须有非 0 stream_id。
     *
     * unary 可以 streamId = 0，
     * 但是 stream open/data/end/cancel 不行。
     */
    if (message_.streamId() == 0) {
        return false;
    }

    /*
     * request_id 建议保留并要求非 0。
     *
     * 好处：
     * - 日志更清楚；
     * - cancel / timeout 更容易关联；
     * - client/server 调试更方便。
     */
    if (message_.requestId() == 0) {
        return false;
    }
    return true;
}

FrameType StreamFrame::frameType() const noexcept {
    return message_.frameType();
}

std::uint16_t StreamFrame::type() const noexcept {
    return message_.type();
}

std::uint32_t StreamFrame::streamId() const noexcept {
    return message_.streamId();
}

std::uint64_t StreamFrame::requestId() const noexcept {
    return message_.requestId();
}

const std::string& StreamFrame::payload() const noexcept {
    return message_.payload();
}

std::size_t StreamFrame::payloadSize() const noexcept {
    return message_.payloadSize();
}

bool StreamFrame::isOpen() const noexcept {
    return message_.frameType() == FrameType::STREAM_OPEN;
}

bool StreamFrame::isData() const noexcept {
    return message_.frameType() == FrameType::STREAM_DATA;
}

bool StreamFrame::isEnd() const noexcept {
    return message_.frameType() == FrameType::STREAM_END;
}

bool StreamFrame::isCancel() const noexcept {
    return message_.frameType() == FrameType::STREAM_CANCEL;
}

bool StreamFrame::isStreamFrame() const noexcept {
    return validStreamType(message_.frameType());
}

const RpcMessage& StreamFrame::message() const noexcept {
    return message_;
}

RpcMessage& StreamFrame::message() noexcept {
    return message_;
}

// 把 StreamFrame 内部包着的 RpcMessage 移出来。
// 因为最终真正要交给 RpcCodec::encode() 或者放进 outResponses
RpcMessage StreamFrame::releaseMessage() noexcept {
    return std::move(message_);
}

//客户端请求打开一个逻辑流。
StreamFrame StreamFrame::makeOpen(std::uint32_t streamId,
                                  std::uint64_t requestId,
                                  std::string payload) {
    return StreamFrame(FrameType::STREAM_OPEN, streamId, requestId,
                       std::move(payload));
}

//某个 stream 的一段数据。
StreamFrame StreamFrame::makeData(std::uint32_t streamId,
                                  std::uint64_t requestId,
                                  std::string payload) {
    return StreamFrame(FrameType::STREAM_DATA, streamId, requestId,
                       std::move(payload));
}

//这个逻辑流结束了。
StreamFrame StreamFrame::makeEnd(std::uint32_t streamId,
                                 std::uint64_t requestId, std::string payload) {
    return StreamFrame(FrameType::STREAM_END, streamId, requestId,
                       std::move(payload));
}

//取消某个 stream。
StreamFrame StreamFrame::makeCancel(std::uint32_t streamId,
                                    std::uint64_t requestId,
                                    std::string payload) {
    return StreamFrame(FrameType::STREAM_CANCEL, streamId, requestId,
                       std::move(payload));
}

std::string_view StreamFrame::kindToString(const StreamFrame& frame) noexcept {
    if (frame.isOpen()) {
        return "STREAM_OPEN";
    }

    if (frame.isData()) {
        return "STREAM_DATA";
    }

    if (frame.isEnd()) {
        return "STREAM_END";
    }

    if (frame.isCancel()) {
        return "STREAM_CANCEL";
    }

    return "NOT_STREAM_FRAME";
}

bool StreamFrame::validStreamType(FrameType type) noexcept {
    switch (type) {
        case FrameType::STREAM_OPEN:
        case FrameType::STREAM_DATA:
        case FrameType::STREAM_END:
        case FrameType::STREAM_CANCEL:
            return true;

        default:
            return false;
    }
}

}  // namespace novanet::rpc