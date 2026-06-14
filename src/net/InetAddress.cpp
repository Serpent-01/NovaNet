#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/net/Endian.h"
#include <netinet/in.h>

using namespace novanet::net;

// 确保 union 内存对齐，保障向下转换为 struct sockaddr* 时的绝对安全
static_assert(sizeof(InetAddress) == sizeof(struct sockaddr_in6), 
              "InetAddress size must match sockaddr_in6");

// 构造服务端监听地址
InetAddress::InetAddress(uint16_t port, bool loopbackOnly, bool ipv6) {
    if (ipv6) {
        addr6_ = {}; // C++17 零初始化
        addr6_.sin6_family = AF_INET6;
        addr6_.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;
        addr6_.sin6_port = sockets::hostToNetwork16(port);
    } else {
        addr_ = {};
        addr_.sin_family = AF_INET;
        in_addr_t ip = loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY;
        addr_.sin_addr.s_addr = sockets::hostToNetwork32(ip);
        addr_.sin_port = sockets::hostToNetwork16(port);
    }
}

// 构造明确 IP 的地址 (通常用于客户端 connect)
InetAddress::InetAddress(std::string_view ip, uint16_t port, bool ipv6) {
    // 显式转为 std::string 保证 '\0' 结尾，防止底层 C API (inet_pton) 越界读取
    std::string ip_str(ip);

    if (ipv6 || ip.find(':') != std::string_view::npos) {
        addr6_ = {};
        sockets::fromIpPort(ip_str.c_str(), port, &addr6_);
    } else {
        addr_ = {};
        sockets::fromIpPort(ip_str.c_str(), port, &addr_);
    }
}

// 获取 IP 字符串
std::string InetAddress::toIp() const {
    char buf[64] = ""; 
    sockets::toIp(buf, sizeof(buf), getSockAddr());
    return buf;
}

// 获取 IP:Port 字符串 (常用于日志打印)
std::string InetAddress::toIpPort() const {
    char buf[64] = "";
    sockets::toIpPort(buf, sizeof(buf), getSockAddr());
    return buf;
}

// TODO C1: 严格依据 family() 分支，解决 IPv4/IPv6 混合读取的语义危险
uint16_t InetAddress::port() const {
    if (family() == AF_INET6) {
        return sockets::networkToHost16(addr6_.sin6_port);
    } else {
        return sockets::networkToHost16(addr_.sin_port);
    }
}

// TODO C1: 严格依据 family() 返回精准的指针
// 虽然首地址一致，但企业级编码规范要求显式的类型安全匹配
const struct sockaddr* InetAddress::getSockAddr() const {
    if (family() == AF_INET6) {
        return reinterpret_cast<const struct sockaddr*>(&addr6_);
    } else {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }
}

// 注入底层数据 (供 accept 返回后填充对端信息)
void InetAddress::setSockAddrInet6(const struct sockaddr_in6 &addr6) {
    addr6_ = addr6;
}