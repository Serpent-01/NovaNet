#include "novanet/net/SocketsOps.h"
#include "novanet/net/Endian.h"
#include "novanet/base/Logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cerrno>
#include <cstdio>

namespace novanet::net::sockets {

/**
 * @brief 创建非阻塞的套接字
 * @details 使用 Linux 特有的 SOCK_NONBLOCK 和 SOCK_CLOEXEC 标志，
 * 一步到位完成非阻塞设置，消除传统 fcntl 的多次系统调用开销，并防止多线程下的 fd 泄露。
 */
int createNonblockingOrDie(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        LOG_FATAL << "sockets::createNonblockingOrDie failed, errno=" << errno;
    }
    return sockfd;
}

/**
 * @brief 绑定地址到套接字
 * @details 动态判断 IPv4/IPv6 协议族以传递精确的 addrlen，防止内存越界读取。
 */
void bindOrDie(int sockfd, const struct sockaddr* addr) {
    socklen_t addrlen = 0;
    if (addr->sa_family == AF_INET) {
        addrlen = static_cast<socklen_t>(sizeof(struct sockaddr_in));
    } else if (addr->sa_family == AF_INET6) {
        addrlen = static_cast<socklen_t>(sizeof(struct sockaddr_in6));
    } else {
        LOG_FATAL << "sockets::bindOrDie failed: Unknown sa_family";
    }

    int ret = ::bind(sockfd, addr, addrlen);
    if (ret < 0) {
        LOG_FATAL << "sockets::bindOrDie failed, errno=" << errno;
    }
}

/**
 * @brief 开始监听
 */
void listenOrDie(int sockfd) {
    int ret = ::listen(sockfd, SOMAXCONN);
    if (ret < 0) {
        LOG_FATAL << "sockets::listenOrDie failed, errno=" << errno;
    }
}

/**
 * @brief 接收新连接
 * @return 成功返回新连接的 fd，失败返回负数（并已按致命/非致命做了精准的 errno 分类）
 */
int accept(int sockfd, struct sockaddr_storage* addr) {
    socklen_t addrlen = static_cast<socklen_t>(sizeof(struct sockaddr_storage));
    
#if VALGRIND || defined (NO_ACCEPT4)
    // 降级使用传统的 accept
    int connfd = ::accept(sockfd, reinterpret_cast<struct sockaddr*>(addr), &addrlen);
    sockets::setNonBlockAndCloseOnExec(connfd);
#else
    // 现代 Linux 高效做法：直接用 accept4 一次性拿 fd 并设置非阻塞标志
    int connfd = ::accept4(sockfd, reinterpret_cast<struct sockaddr*>(addr),
                           &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif

    if (connfd < 0) {
        int savedErrno = errno;
        
        // 【核心修改】：删掉了原来的 LOG_SYSERR 大喇叭！
        // 让下面的 switch 来做精准的哑巴和报警器
        
        switch (savedErrno) {
            case EAGAIN:
            case ECONNABORTED:
            case EINTR:
            case EPROTO: 
            case EPERM:
            case EMFILE: // 我们之前写的防雷机制就是靠捕获这个
                // 【消音区】：这些是网络底层的暂态错误，完全正常！
                // 什么都不用打印，默默恢复 errno 即可
                errno = savedErrno;
                break;
            case EBADF:
            case EFAULT:
            case EINVAL:
            case ENFILE:
            case ENOBUFS:
            case ENOMEM:
            case ENOTSOCK:
            case EOPNOTSUPP:
                // 这些是致命错误，说明 fd 坏了或者内存爆了，直接 FATAL 终结进程
                LOG_SYSFATAL << "unexpected error of ::accept " << savedErrno;
                break;
            default:
                LOG_SYSFATAL << "unknown error of ::accept " << savedErrno;
                break;
        }
    }
    return connfd;
}

ssize_t read(int sockfd, void* buf, size_t count) {
    return ::read(sockfd, buf, count);
}

ssize_t write(int sockfd,const void* buf, size_t count) {
    return ::write(sockfd, buf, count);
}

void close(int sockfd) {
    if (::close(sockfd) < 0) {
        LOG_SYSERR << "sockets::close failed on fd=" << sockfd;
    }
}

/**
 * @brief 忽略 SIGPIPE 信号
 * @details 在往一个已经对端关闭的 socket 写数据时，OS 会默认发送 SIGPIPE 杀死进程。
 * 作为服务器，我们必须忽略此信号，改为处理 write 返回的 -1 和 EPIPE 错误。
 */
void ignoreSigPipe() {
    struct sigaction sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    ::sigaction(SIGPIPE, &sa, nullptr); // 使用更严谨的 sigaction 替代 signal
}

/**
 * @brief 允许重用本地地址和端口
 * @details 解决服务器重启时处于 TIME_WAIT 状态的连接占用端口的问题
 */
void setReuseAddr(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, static_cast<socklen_t>(sizeof optval));
}

/**
 * @brief 允许端口复用 (Linux 3.9+ 核心特性)
 * @details 允许多个线程/进程绑定到同一个端口，由内核级进行负载均衡
 */
void setReusePort(int sockfd, bool on) {
#ifdef SO_REUSEPORT
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, static_cast<socklen_t>(sizeof optval));
#endif
}

/**
 * @brief 禁用 Nagle 算法
 * @details 避免小包凑大包带来的延迟，这对追求极速响应的 RPC 框架至关重要
 */
void setTcpNoDelay(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, static_cast<socklen_t>(sizeof optval));
}

/**
 * @brief 将 sockaddr 转换为人类可读的 IP:Port 字符串
 */
void toIpPort(char* buf, size_t size, const struct sockaddr* addr) {
    if (addr->sa_family == AF_INET6) {
        buf[0] = '[';
        toIp(buf + 1, size - 1, addr);
        size_t end = ::strlen(buf);
        const struct sockaddr_in6* addr6 = sockaddr_in6_cast(addr);
        uint16_t port = networkToHost16(addr6->sin6_port);
        ::snprintf(buf + end, size - end, "]:%u", port);
        return;
    }
    toIp(buf, size, addr);
    size_t end = ::strlen(buf);
    const struct sockaddr_in* addr4 = sockaddr_in_cast(addr);
    uint16_t port = networkToHost16(addr4->sin_port);
    ::snprintf(buf + end, size - end, ":%u", port);
}

/**
 * @brief 将 sockaddr 转换为人类可读的纯 IP 字符串
 */
void toIp(char* buf, size_t size, const struct sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* addr4 = sockaddr_in_cast(addr);
        ::inet_ntop(AF_INET, &addr4->sin_addr, buf, static_cast<socklen_t>(size));
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = sockaddr_in6_cast(addr);
        ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, static_cast<socklen_t>(size));
    }
}

/**
 * @brief 将字符串 IP 和主机字节序端口打包入 IPv4 结构体
 */
void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr) {
    addr->sin_family = AF_INET;
    addr->sin_port = sockets::hostToNetwork16(port);
    if (::inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
        LOG_SYSERR << "sockets::fromIpPort IPv4 error, ip=" << ip;
    }
}

/**
 * @brief 将字符串 IP 和主机字节序端口打包入 IPv6 结构体
 */
void fromIpPort(const char *ip, uint16_t port, struct sockaddr_in6 *addr) {
    addr->sin6_family = AF_INET6;
    addr->sin6_port = hostToNetwork16(port);
    if (::inet_pton(AF_INET6, ip, &addr->sin6_addr) <= 0) {
        LOG_SYSERR << "sockets::fromIpPort IPv6 error, ip=" << ip;
    }
}

} // namespace novanet::net::sockets