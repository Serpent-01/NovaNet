#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::rpc {

RpcMessage::RpcMessage(RpcHeader header, std::string payload)
    : header_(header), payload_(std::move(payload)) {
    refreshTotalLen();
}

RpcMessage::RpcMessage(FrameType type, uint32_t streamId, uint64_t requestId,
                       std::string payload, uint16_t flags)
    : payload_(std::move(payload)) {
    header_.type = static_cast<uint16_t>(type);
    header_.flags = flags;
    header_.streamId = streamId;
    header_.requestId = requestId;

    refreshTotalLen();
}

RpcMessage RpcMessage::make(FrameType type, uint32_t streamId,
                            uint64_t requestId, std::string payload,
                            uint16_t flags) {
    return RpcMessage(type, streamId, requestId, std::move(payload), flags);
}

const RpcHeader& RpcMessage::header() const noexcept { return header_; }

const std::string& RpcMessage::payload() const noexcept { return payload_; }

uint32_t RpcMessage::totalLen() const noexcept { return header_.totalLen; }

uint16_t RpcMessage::type() const noexcept { return header_.type; }

FrameType RpcMessage::frameType() const noexcept {
    return toFrameType(header_.type);
}

uint16_t RpcMessage::flags() const noexcept { return header_.flags; }

uint32_t RpcMessage::streamId() const noexcept { return header_.streamId; }

uint64_t RpcMessage::requestId() const noexcept { return header_.requestId; }

std::size_t RpcMessage::payloadSize() const noexcept { return payload_.size(); }

bool RpcMessage::empty() const noexcept {
    return header_.totalLen == 0 && payload_.empty();
}

bool RpcMessage::valid() const noexcept {
    if (!header_.isValid()) {
        return false;
    }
    if (payload_.size() >
        RpcHeader::kMaxFrameSize - RpcHeader::kFixedHeaderLen) {
        return false;
    }

    const auto expectedTotalLen =
        static_cast<uint32_t>(RpcHeader::kFixedHeaderLen + payload_.size());

    return header_.totalLen == expectedTotalLen;
}

void RpcMessage::setPayload(std::string payload) {
    payload_ = std::move(payload);
    refreshTotalLen();
}

void RpcMessage::clear() noexcept {
    header_ = RpcHeader{};
    payload_.clear();
}

void RpcMessage::refreshTotalLen() noexcept {
    if (payload_.size() >
        RpcHeader::kMaxFrameSize - RpcHeader::kFixedHeaderLen) {
        header_.totalLen = RpcHeader::kMaxFrameSize + 1;
        return;
    }
    header_.totalLen =
        static_cast<uint32_t>(payload_.size() + RpcHeader::kFixedHeaderLen);
}

}  // namespace novanet::rpc