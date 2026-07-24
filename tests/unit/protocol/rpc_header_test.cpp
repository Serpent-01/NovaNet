#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcHeader.h"

namespace novanet::rpc {
namespace {
std::uint16_t raw(FrameType type) {
    return static_cast<std::uint16_t>(type);
}

RpcHeader makeHeader(std::uint16_t type,
                     std::uint32_t totalLen = RpcHeader::kFixedHeaderLen,
                     std::uint16_t flags = 0, std::uint32_t streamId = 0,
                     std::uint64_t requestId = 1) {
    RpcHeader header;
    header.type = type;
    header.totalLen = totalLen;
    header.flags = flags;
    header.streamId = streamId;
    header.requestId = requestId;
    return header;
}

TEST(RpcHeaderTest, FixedHeaderLengthShouldBe20Bytes) {
    EXPECT_EQ(RpcHeader::kFixedHeaderLen, 20u);
}

TEST(RpcHeaderTest, ValidUnaryRequestShouldPassValidation) {
    RpcHeader header = makeHeader(raw(FrameType::UNARY_REQUEST),
                                  RpcHeader::kFixedHeaderLen, 0, 0, 1001);
    EXPECT_TRUE(header.isValid());
}

TEST(RpcHeaderTest, HeaderWithTooLargeTotalLenShouldFailValidation) {
    RpcHeader header = makeHeader(raw(FrameType::UNARY_REQUEST),
                                  RpcHeader::kMaxFrameSize + 1, 0, 0, 1006);
    EXPECT_FALSE(header.isValid());
}

//测试未知 FrameType
TEST(RpcHeaderTest, HeaderWithUnknownFrameTypeShouldFailValidation) {
    RpcHeader header = makeHeader(9999, RpcHeader::kFixedHeaderLen, 0, 0, 1007);
    EXPECT_FALSE(header.isValid());
}

//测合法 HEARTBEAT_PING
TEST(RpcHeaderTest, ValidHeartbeatPingShouldPassValidation) {
    RpcHeader header = makeHeader(raw(FrameType::HEARTBEAT_PING),
                                  RpcHeader::kFixedHeaderLen, 0, 0, 0);
    EXPECT_TRUE(header.isValid());
}

}  // namespace
}  // namespace novanet::rpc