#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>


#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcHeader.h"

namespace novanet::rpc{

class RpcMessage final{

public:
    RpcMessage() = default;
    


    /*
     * 用于 RpcCodec 解码之后构造消息：
     *
     *     Buffer -> RpcHeader + payload -> RpcMessage
     */
    RpcMessage(RpcHeader header,std::string payload);


    /*
     * 用于业务层主动构造要发送的消息：
     *
     *     RpcDispatcher / RpcChannel / StreamManager -> RpcMessage
     */
    RpcMessage(FrameType type,uint32_t streamId,
                uint64_t requestId,std::string payload = {},
                uint16_t flags = 0);


     /*
     * 命名工厂函数。
     * 它只是转调构造函数；
     * 后面可以继续扩展出：
     *
     *     makeUnaryRequest()
     *     makeUnaryResponse()
     *     makeStreamOpen()
     *     makeStreamData()
     *     makeStreamEnd()
     *     makeHeartbeatPing()
     */
    static RpcMessage make(FrameType type,
                           uint32_t streamId,
                           uint64_t requestId,
                           std::string payload = {},
                           uint16_t flags = 0);
    

    [[nodiscard]] const RpcHeader& header() const noexcept;
    [[nodiscard]] const std::string& payload() const noexcept;
    
    [[nodiscard]] uint32_t totalLen() const noexcept;
    [[nodiscard]] uint16_t type() const noexcept;
    [[nodiscard]] FrameType frameType() const noexcept;
    [[nodiscard]] uint16_t flags() const noexcept;
    [[nodiscard]] uint32_t streamId() const noexcept;
    [[nodiscard]] uint64_t requestId() const noexcept;
    [[nodiscard]] std::size_t payloadSize() const noexcept;

    // empty 表示“还没有承载任何有效消息”，不是“payload 为空”。
    [[nodiscard]] bool empty() const noexcept;
    

    // valid 表示 header 和 payload 长度语义一致。
    [[nodiscard]] bool valid() const noexcept;
    

    void setPayload(std::string payload);
    void clear() noexcept;
private:
    void refreshTotalLen() noexcept;
private:
    RpcHeader header_{};
    std::string payload_;                         
};


} //namespace novanet::rpc