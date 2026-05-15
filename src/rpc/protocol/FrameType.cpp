#include "novanet/rpc/protocol/FrameType.h"
#include <cstdint>

namespace novanet::rpc{


bool isValidFrameType(uint16_t type) noexcept {
    switch (static_cast<FrameType>(type)) {
    case FrameType::UNARY_REQUEST:
    case FrameType::UNARY_RESPONSE:
    case FrameType::STREAM_OPEN:
    case FrameType::STREAM_DATA:
    case FrameType::STREAM_END:
    case FrameType::STREAM_CANCEL:
    case FrameType::HEARTBEAT_PING:
    case FrameType::HEARTBEAT_PONG:
    case FrameType::ERROR_FRAME:
        return true;

    case FrameType::UNKNOWN:
    default:
        return false;
    }
}


FrameType toFrameType(uint16_t type) noexcept {
    if (!isValidFrameType(type)) {
        return FrameType::UNKNOWN;
    }

    return static_cast<FrameType>(type);
}

bool isUnaryFrameType(FrameType type) noexcept {
    switch (type) {
    case FrameType::UNARY_REQUEST:
    case FrameType::UNARY_RESPONSE:
        return true;

    default:
        return false;
    }
}

bool isStreamFrameType(FrameType type) noexcept {
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


bool isHeartbeatFrameType(FrameType type) noexcept {
    switch (type) {
    case FrameType::HEARTBEAT_PING:
    case FrameType::HEARTBEAT_PONG:
        return true;

    default:
        return false;
    }
}

std::string_view frameTypeToString(FrameType type) noexcept {
    switch (type) {
    case FrameType::UNKNOWN:
        return "UNKNOWN";

    case FrameType::UNARY_REQUEST:
        return "UNARY_REQUEST";

    case FrameType::UNARY_RESPONSE:
        return "UNARY_RESPONSE";

    case FrameType::STREAM_OPEN:
        return "STREAM_OPEN";

    case FrameType::STREAM_DATA:
        return "STREAM_DATA";

    case FrameType::STREAM_END:
        return "STREAM_END";

    case FrameType::STREAM_CANCEL:
        return "STREAM_CANCEL";

    case FrameType::HEARTBEAT_PING:
        return "HEARTBEAT_PING";

    case FrameType::HEARTBEAT_PONG:
        return "HEARTBEAT_PONG";

    case FrameType::ERROR_FRAME:
        return "ERROR_FRAME";

    default:
        return "INVALID_TYPE";
    }
}

};