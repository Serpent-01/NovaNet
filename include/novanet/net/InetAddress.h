#pragma once
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h> // socklen_t

namespace novanet::net {

class InetAddress {
public:
    /**
     * @brief 服务端专用的快捷构造函数 (隐式 IP)
     *
     * 设计意图：在编写 TCP 服务端时，通常不硬编码具体的 IP 地址，以适应多网卡环境。
     * @param port 监听端口号。
     * - 传入具体端口（如 8080）用于常规服务绑定。
     * - 传入 0 时，操作系统会在 bind() 阶段自动分配一个当前可用的临时端口 (Ephemeral Port)。
     * @param loopbackOnly 决定服务器的网络可见性 (安全与访问控制)：
     * - false (默认): 监听所有网卡。底层将 IP 自动设为 INADDR_ANY (0.0.0.0)。
     * 允许接收来自公网或局域网所有物理网卡的请求。
     * - true: 仅监听回环地址。底层将 IP 自动设为 INADDR_LOOPBACK (127.0.0.1)。
     * 数据包不流出物理网卡，仅限同一台物理机上的本机进程间通信，提供网络层面的硬隔离。
     * @param ipv6 是否使用 IPv6 (底层对应使用 sockaddr_in6)。
     */
    explicit InetAddress(uint16_t port = 0,bool loopbackOnly = false,bool ipv6 = false);

    /**
     * @brief 提供精确 IP + Port 寻址的构造函数
     *
     * 设计意图：通常用于 Client 端或具有多块网卡的特定 Server 端。
     * 适用场景：
     * 1. 客户端 (Client)：发起 connect() 时，必须明确指定目标服务器的具体 IP 和端口。
     * 2. 多网卡服务端 (Server)：服务器同时具备内外网网卡时，精确绑定单一网卡 (如仅监听内网 IP)，拒绝其他网卡的流量。
     *
     * @param ip C++17 引入的 std::string_view (字符串视图)。
     * 技术细节：它内部仅包含一个 const char* 指针和一个 size_t 长度。
     * 性能优势：避免了传参时的临时字符串拷贝开销。(注：底层 C API 需 \0 结尾，内部会做一次安全的转换)。
     * @param port 目标或绑定的端口号。
     * @param ipv6 是否解析为 IPv6 地址。
     */
    InetAddress(std::string_view ip,uint16_t port , bool ipv6 = false);

    explicit InetAddress(const struct sockaddr_in& addr) : addr_(addr){}
    explicit InetAddress(const struct sockaddr_in6& addr6) : addr6_(addr6){}



    ~InetAddress() = default;
    InetAddress(const InetAddress&) = default;
    InetAddress& operator=(const InetAddress&) = default;

    // 因为 sockaddr_in 和 sockaddr_in6 的首个字段都是 sa_family_t，读取 addr_.sin_family 是安全的
    sa_family_t family() const { return addr_.sin_family; }

    [[nodiscard]] socklen_t length() const noexcept {
        return (family() == AF_INET6)
            ? static_cast<socklen_t>(sizeof(addr6_))
            : static_cast<socklen_t>(sizeof(addr_));
    }

    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t port() const;

    const struct sockaddr* getSockAddr() const;
    void setSockAddrInet6(const struct sockaddr_in6 &addr6);
private:
    union{
        struct sockaddr_in addr_;
        struct sockaddr_in6 addr6_;
    };
};

}  // namespace novanet::net
