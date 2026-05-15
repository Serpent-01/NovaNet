#pragma once

#include <string>

#include "novanet/rpc/protocol/RpcHeader.h"
#include "novanet/rpc/protocol/RpcMessage.h"

namespace novanet::net {

class Buffer;

}

namespace novanet::rpc {
class RpcCodec final {
public:
    enum class DecodeStatus {
        kOk,        //成功解出一帧
        kNeedMore,  // 半包，Buffer 数据还不够
        kInvalid    // 非法帧，调用方应关闭连接或返回协议错误
    };

public:
    RpcCodec() = default;

    RpcCodec(const RpcCodec&) = delete;
    RpcCodec& operator=(const RpcCodec&) = delete;

    RpcCodec(RpcCodec&&) = default;
    RpcCodec& operator=(RpcCodec&&) = default;

    ~RpcCodec() = default;

    /*
     * 从 Buffer 中尝试解析一条完整 RpcMessage。
     *
     * 语义：
     * 1. 如果 Buffer 中连固定头都不够，返回 kNeedMore。
     * 2. 如果固定头非法，返回 kInvalid。
     * 3. 如果整帧未到齐，返回 kNeedMore。
     * 4. 如果整帧完整，则填充 out，并移动 Buffer 读指针。
     *
     * 注意：
     * - 半包时绝对不能 retrieve。
     * - 成功解码后才可以 retrieve(header.totalLen)。
     * - 一次只解一帧，粘包由外层 while 循环继续调用。
     */
    [[nodiscard]] DecodeStatus tryDecode(novanet::net::Buffer& buffer,
                                         RpcMessage& out) const;

    /*
     * 将 RpcMessage 编码并追加到 Buffer。
     *
     * 返回 false 表示 msg 本身不合法：
     * - type 非法
     * - totalLen 与 payload size 不匹配
     * - streamId / requestId 不符合帧语义
     * - 超过最大帧大小
     */
    [[nodiscard]] bool encode(const RpcMessage& msg,
                              novanet::net::Buffer& out) const;

    /*
     * 辅助函数：编码成 std::string。
     *
     * 方便单元测试：
     * - RpcHeaderTest
     * - RpcCodecTest
     */
    [[nodiscard]] bool encodeToString(const RpcMessage& msg,
                                      std::string& out) const;
};
}  // namespace novanet::rpc