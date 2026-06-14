#pragma once

#include <cstdint>  // 优化：使用 C++ 标准的 <cstdint>
#include <endian.h> // Linux 标准字节序转换头文件

namespace novanet::net::sockets {

// 封装 64 位转换（用于处理大数据量或长整型协议字段）
[[nodiscard]] inline uint64_t hostToNetwork64(uint64_t host64) {
    return htobe64(host64);
}

// 封装 32 位转换（用于处理 IPv4 地址等）
[[nodiscard]] inline uint32_t hostToNetwork32(uint32_t host32) {
    return htobe32(host32);
}

// 封装 16 位转换（用于处理端口号）
[[nodiscard]] inline uint16_t hostToNetwork16(uint16_t host16) {
    return htobe16(host16);
}

// --- 反向转换 (Network to Host) ---

[[nodiscard]] inline uint64_t networkToHost64(uint64_t net64) {
    return be64toh(net64);
}

[[nodiscard]] inline uint32_t networkToHost32(uint32_t net32) {
    return be32toh(net32);
}

[[nodiscard]] inline uint16_t networkToHost16(uint16_t net16) {
    return be16toh(net16);
}

} // namespace novanet::net::sockets