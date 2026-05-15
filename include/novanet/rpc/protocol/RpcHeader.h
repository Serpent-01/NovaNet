#pragma once
#include <cstdint>
#include <string>

namespace novanet::rpc{

class RpcHeader{
public:
    /*
     * 固定头格式，共 20 字节：
     *
     * | total_len(4) | type(2) | flags(2) | stream_id(4) | request_id(8) |
     *
     * totalLen 的语义：
     *
     *     totalLen = kFixedHeaderLen + payload.size()
     *
     * 也就是说，totalLen 表示整帧长度，包含固定头，也包含 payload。
     */
    
    
    //字段定义 (共20字节)
    uint32_t totalLen{0};
    uint16_t type{0};
    uint16_t flags{0};
    uint32_t streamId{0};
    uint64_t requestId{0};

    //静态常量，固定头部的长度
    static constexpr uint32_t kFixedHeaderLen = 20;

    //单个frame 最大不超过 16MB
    static constexpr uint32_t kMaxFrameSize = 16 * 1024 * 1024;


    
    [[nodiscard]] bool isValid() const noexcept;

    [[nodiscard]] uint32_t payloadLen() const noexcept;


    // 将固定头编码为网络字节序，并追加到 out。
    void encodeTo(std::string& out) const;



    // 从 data 中解析固定头。
    //
    // 注意：
    // 返回 false 可能表示：
    //   1. data == nullptr
    //   2. len < kFixedHeaderLen，半包
    //   3. header 字段非法
    //
    // RpcCodec 需要先判断 readableBytes() 是否够 20 字节，
    // 再调用 decodeFrom，这样才能区分半包和非法头。
    static bool decodeFrom(const char* data,size_t len,RpcHeader& out) noexcept;
};


}//namespace novanet::rpc