#include <cassert>
#include <iostream>
#include <string>

#include "novanet/net/Buffer.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "rpc_meta.pb.h"

int main() {
    using namespace novanet::rpc;
    using namespace novanet::rpc::meta;
    // 1. 构造 UnaryRequestMeta
    UnaryRequestMeta meta;
    meta.set_service_name("CalculatorService");
    meta.set_method_name("Add");
    meta.set_request_payload("fake_add_request_bytes");

    std::string metaBytes;
    assert(meta.SerializeToString(&metaBytes));
    assert(!metaBytes.empty());

    // 2. 把 protobuf payload 放进 RpcMessage
    RpcMessage request(FrameType::UNARY_REQUEST,
                       0,    // unary 第一版 streamId 可以为 0
                       1001, // requestId 必须非 0
                       metaBytes);

    assert(request.valid());

    // 3. RpcCodec 编码到 Buffer
    novanet::net::Buffer buffer;
    RpcCodec codec;

    const bool encoded = codec.encode(request, buffer);
    assert(encoded);

    // 4. 从 Buffer 解码回 RpcMessage
    RpcMessage decoded;
    const auto status = codec.tryDecode(buffer, decoded);

    assert(status == RpcCodec::DecodeStatus::kOk);
    assert(decoded.valid());
    assert(decoded.frameType() == FrameType::UNARY_REQUEST);
    assert(decoded.streamId() == 0);
    assert(decoded.requestId() == 1001);

    // 5. 再从 decoded.payload() 解析回 UnaryRequestMeta
    UnaryRequestMeta parsedMeta;
    const bool parsed = parsedMeta.ParseFromString(decoded.payload());
    assert(parsed);

    assert(parsedMeta.service_name() == "CalculatorService");
    assert(parsedMeta.method_name() == "Add");
    assert(parsedMeta.request_payload() == "fake_add_request_bytes");

    std::cout << "[PASS] RpcCodecMetaTest passed.\n";
    return 0;
}