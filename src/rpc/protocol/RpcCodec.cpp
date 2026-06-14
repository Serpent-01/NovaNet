#include "novanet/rpc/protocol/RpcCodec.h"

#include <cstddef>
#include <string>
#include <utility>

#include "novanet/net/Buffer.h"

namespace novanet::rpc {
RpcCodec::DecodeStatus RpcCodec::tryDecode(novanet::net::Buffer& buffer,
                                           RpcMessage& out) const {
    /*
     * 第一步：固定头是否到齐？
     *
     * TCP 是字节流。
     * 这里可能只收到 1~19 字节，所以不能强行 decode header。
     */
    if (buffer.readableBytes() < RpcHeader::kFixedHeaderLen) {
        return DecodeStatus::kNeedMore;
    }

    /*
     * 第二步：只 peek，不 retrieve。
     *
     * 因为现在还不知道整帧是否到齐。
     * 如果这里提前 retrieve header，后面发现 payload 没到齐，就会丢包。
     */
    RpcHeader header;
    if (!RpcHeader::decodeFrom(buffer.peek(), buffer.readableBytes(), header)) {
        return DecodeStatus::kInvalid;
    }

    /*
     * 第三步：检查整帧是否到齐。
     *
     * header.totalLen 的语义是：
     *
     *     totalLen = fixed_header_len + payload_len
     *
     * 如果 readableBytes() < totalLen，说明 payload 还没收完整。
     * 这就是半包。
     *
     * 半包时必须返回 kNeedMore，并且不能移动 Buffer 读指针。
     */
    if (buffer.readableBytes() < header.totalLen) {
        return DecodeStatus::kNeedMore;
    }

    /*
     * 第四步：提取 payload。
     *
     * payload 起点：
     *
     *     buffer.peek() + RpcHeader::kFixedHeaderLen
     *
     * payload 长度：
     *
     *     header.totalLen - RpcHeader::kFixedHeaderLen
     */
    const std::size_t payloadLen = header.payloadLen();

    std::string payload;
    if (payloadLen > 0) {
        payload.assign(buffer.peek() + RpcHeader::kFixedHeaderLen, payloadLen);
    }
    RpcMessage msg(header, std::move(payload));

    /*
     * 第五步：兜底校验。
     *
     * 理论上 RpcHeader::decodeFrom 已经校验了 header；
     * 这里再校验一次 RpcMessage，确保：
     *
     *     header.totalLen == fixed_header_len + payload.size()
     */

    if (!msg.valid()) {
        return DecodeStatus::kInvalid;
    }
    /*
     * 第六步：确认整帧完整且合法后，才移动读指针。
     *
     * 这是整个 RpcCodec 最关键的点。
     */
    buffer.retrieve(header.totalLen);

    out = std::move(msg);
    return DecodeStatus::kOk;
}

bool RpcCodec::encode(const RpcMessage& msg, novanet::net::Buffer& out) const {
    std::string bytes;
    if (!encodeToString(msg, bytes)) {
        return false;
    }
    if (!bytes.empty()) {
        out.append(bytes.data(), bytes.size());
    }
    return true;
}

bool RpcCodec::encodeToString(const RpcMessage& msg, std::string& out) const {
    if (!msg.valid()) {
        return false;
    }
    out.clear();
    out.reserve(msg.totalLen());
    msg.header().encodeTo(out);

    if (!msg.payload().empty()) {
        out.append(msg.payload());
    }
    return out.size() == msg.totalLen();
}

}  // namespace novanet::rpc