#include "novanet/rpc/protocol/RpcHeader.h"
#include "novanet/rpc/protocol/FrameType.h"

namespace novanet::rpc {
namespace {

void appendU16BE(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendU32BE(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendU64BE(std::string& out, uint64_t value) {
    out.push_back(static_cast<char>((value >> 56) & 0xFF));
    out.push_back(static_cast<char>((value >> 48) & 0xFF));
    out.push_back(static_cast<char>((value >> 40) & 0xFF));
    out.push_back(static_cast<char>((value >> 32) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

uint16_t readU16BE(const char* data) noexcept {
    return (static_cast<uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
           (static_cast<uint16_t>(static_cast<unsigned char>(data[1])));
}

uint32_t readU32BE(const char* data) noexcept {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[3])));
}

uint64_t readU64BE(const char* data) noexcept {
    return (static_cast<uint64_t>(static_cast<unsigned char>(data[0])) << 56) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[1])) << 48) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[2])) << 40) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[3])) << 32) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[4])) << 24) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[5])) << 16) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[6])) << 8) |
           (static_cast<uint64_t>(static_cast<unsigned char>(data[7])));
}

} // namespace

bool RpcHeader::isValid() const noexcept {
    if (totalLen < kFixedHeaderLen) {
        return false;
    }

    if (totalLen > kMaxFrameSize) {
        return false;
    }

    const FrameType frameType = toFrameType(type);
    if (frameType == FrameType::UNKNOWN) {
        return false;
    }

    /*
     *
     * 1. HEARTBEAT_PING / HEARTBEAT_PONG 是连接级控制帧：
     *      streamId  == 0
     *      requestId == 0
     *
     * 2. UNARY_REQUEST / UNARY_RESPONSE：
     *      requestId 必须非 0
     *      streamId 可以为 0
     *
     * 3. STREAM_OPEN / DATA / END / CANCEL：
     *      streamId  必须非 0
     *      requestId 必须非 0
     *
     * 4. ERROR_FRAME：
     *      可以是请求级错误，也可以是连接级协议错误。
     *      因此这里不强行要求 requestId 非 0。
     */
    if (isHeartbeatFrameType(frameType)) {
        return streamId == 0 && requestId == 0;
    }

    if (isUnaryFrameType(frameType)) {
        return requestId != 0;
    }

    if (isStreamFrameType(frameType)) {
        return streamId != 0 && requestId != 0;
    }

    if (frameType == FrameType::ERROR_FRAME) {
        return true;
    }

    return false;
}

uint32_t RpcHeader::payloadLen() const noexcept {
    if (totalLen < kFixedHeaderLen) {
        return 0;
    }

    return totalLen - kFixedHeaderLen;
}

void RpcHeader::encodeTo(std::string& out) const {
    out.reserve(out.size() + kFixedHeaderLen);

    appendU32BE(out, totalLen);
    appendU16BE(out, type);
    appendU16BE(out, flags);
    appendU32BE(out, streamId);
    appendU64BE(out, requestId);
}

bool RpcHeader::decodeFrom(const char* data,
                           std::size_t len,
                           RpcHeader& out) noexcept {
    if (data == nullptr || len < kFixedHeaderLen) {
        return false;
    }

    RpcHeader tmp;
    tmp.totalLen = readU32BE(data);
    tmp.type = readU16BE(data + 4);
    tmp.flags = readU16BE(data + 6);
    tmp.streamId = readU32BE(data + 8);
    tmp.requestId = readU64BE(data + 12);

    if (!tmp.isValid()) {
        return false;
    }

    out = tmp;
    return true;
}

} // namespace novanet::rpc