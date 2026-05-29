#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::rpc {
class StreamFrame final {
public:
    StreamFrame() = default;

    /*
     * 从已有 RpcMessage 包装成 StreamFrame。
     *
     * 注意：
     * 这里不强行 throw。
     * 是否合法由 valid() 判断。
     */
    explicit StreamFrame(RpcMessage message);

    /*
     * 直接构造一个 stream frame。
     *
     * type 必须是：
     * - STREAM_OPEN
     * - STREAM_DATA
     * - STREAM_END
     * - STREAM_CANCEL
     */
    StreamFrame(FrameType type, std::uint32_t streamId, std::uint64_t requestId,
                std::string payload);

    ~StreamFrame() = default;

    StreamFrame(const StreamFrame&) = default;
    StreamFrame& operator=(const StreamFrame&) = default;

    StreamFrame(StreamFrame&&) = default;
    StreamFrame& operator=(StreamFrame&&) = default;

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] FrameType frameType() const noexcept;
    [[nodiscard]] std::uint16_t type() const noexcept;
    [[nodiscard]] std::uint32_t streamId() const noexcept;
    [[nodiscard]] std::uint64_t requestId() const noexcept;

    [[nodiscard]] const std::string& payload() const noexcept;
    [[nodiscard]] std::size_t payloadSize() const noexcept;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isData() const noexcept;
    [[nodiscard]] bool isEnd() const noexcept;
    [[nodiscard]] bool isCancel() const noexcept;

    /*
     * 是否属于 stream frame。
     *
     * 只判断 frameType，不检查 streamId/requestId。
     */
    [[nodiscard]] bool isStreamFrame() const noexcept;

    /*
     * 交回 RpcMessage。
     *
     * 用于：
     * - RpcCodec::encode()
     * - RpcDispatcher 输出响应
     */
    [[nodiscard]] const RpcMessage& message() const noexcept;
    [[nodiscard]] RpcMessage& message() noexcept;

    /*
     * 移出内部 RpcMessage。
     *
     * 用于需要转移所有权的场景。
     */
    [[nodiscard]] RpcMessage releaseMessage() noexcept;

    /*
     * 工厂函数：更清晰地表达语义。
     */
    [[nodiscard]] static StreamFrame makeOpen(std::uint32_t streamId,
                                              std::uint64_t requestId,
                                              std::string payload);

    [[nodiscard]] static StreamFrame makeData(std::uint32_t streamId,
                                              std::uint64_t requestId,
                                              std::string payload);

    [[nodiscard]] static StreamFrame makeEnd(std::uint32_t streamId,
                                             std::uint64_t requestId,
                                             std::string payload = "");

    [[nodiscard]] static StreamFrame makeCancel(std::uint32_t streamId,
                                                std::uint64_t requestId,
                                                std::string payload = "");
    [[nodiscard]] static std::string_view kindToString(
        const StreamFrame& frame) noexcept;

private:
    [[nodiscard]] static bool validStreamType(FrameType type) noexcept;

private:
    RpcMessage message_;
};
}  // namespace novanet::rpc