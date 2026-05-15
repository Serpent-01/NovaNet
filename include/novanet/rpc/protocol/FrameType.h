#pragma once

#include <cstdint>
#include <string_view>

namespace novanet::rpc{

enum class FrameType : uint16_t{
    UNKNOWN = 0,

    // Unary RPC：一次请求，一次响应
    UNARY_REQUEST = 1,
    UNARY_RESPONSE = 2,

    // Streaming RPC：一个 TCP 连接上承载多个逻辑流
    STREAM_OPEN  = 3,
    STREAM_DATA = 4,
    STREAM_END = 5,
    STREAM_CANCEL = 6,

    // 连接级控制帧
    HEARTBEAT_PING = 7,
    HEARTBEAT_PONG = 8,

    //协议级错误
    ERROR_FRAME = 9
};

// 从网络收到的是 uint16_t，需要先判断是否合法。
[[nodiscard]] bool isValidFrameType(uint16_t type) noexcept;

// 安全地把 uint16_t 转成 FrameType。非法值返回 UNKNOWN。
[[nodiscard]] FrameType toFrameType(uint16_t type) noexcept;

[[nodiscard]] bool isUnaryFrameType(FrameType type) noexcept;

[[nodiscard]] bool isStreamFrameType(FrameType type) noexcept;
[[nodiscard]] bool isHeartbeatFrameType(FrameType type) noexcept;


//用于日志打印：将数字转化为人类可读的字符串
[[nodiscard]] std::string_view frameTypeToString(FrameType type) noexcept;

}//namespace novanet::rpc