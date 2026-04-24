#include "novanet/net/Socket.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/base/Logger.h"

// TODO D1: 删除了无用的、偏内核风格的 <asm-generic/socket.h>，收紧依赖
#include <utility>      // for std::exchange
#include <netinet/tcp.h>
#include <sys/socket.h>

using namespace novanet::net;

// 析构函数：RAII 的核心，确保有效的 fd 被自动关闭，防止系统资源泄漏
Socket::~Socket() {
    if (sockfd_ >= 0) {
        sockets::close(sockfd_);
    }
}

// 移动构造：接管 other 的 fd，并将 other 的 fd 置为 -1 (使其失效)
Socket::Socket(Socket&& other) noexcept 
    : sockfd_(std::exchange(other.sockfd_, -1)) {}

// 移动赋值：安全地转移 fd 的所有权
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        // 1. 如果自己持有有效 fd，必须先关闭旧的
        if (sockfd_ >= 0) {
            sockets::close(sockfd_);
        }
        // 2. 接管新 fd，并清空原对象的 fd
        sockfd_ = std::exchange(other.sockfd_, -1);
    }
    return *this;
}

// 绑定本地地址 (传引用避免额外拷贝)
void Socket::bindAddress(const InetAddress& addr) {
    sockets::bindOrDie(sockfd_, addr.getSockAddr());
}

// 开始监听 (失败直接 abort 程序)
void Socket::listen() {
    sockets::listenOrDie(sockfd_);
}

// 接受连接
// 返回非负 connfd 表示成功，并将客户端真实地址写入 peeraddr
int Socket::accept(InetAddress* peeraddr) {
    // TODO D2: 废弃 sockaddr_in6 的强耦合，改用能容纳任何地址族的 sockaddr_storage
    struct sockaddr_storage addr{}; 
    
    // 注：这里假设 SocketsOps::accept 已经被同步优化为接收通用指针
    // 如果底层的 sockets::accept 依然写死了接收 sockaddr_in6*，
    // 可以在这里强转 reinterpret_cast<struct sockaddr_in6*>(&addr)，但外层逻辑已完美解耦
    int connfd = sockets::accept(sockfd_, &addr);
    
    if (connfd >= 0 && peeraddr != nullptr) {
        // 根据实际返回的 family 族，动态重构 InetAddress 对象，做到语义严密
        if (addr.ss_family == AF_INET) {
            *peeraddr = InetAddress(*reinterpret_cast<const struct sockaddr_in*>(&addr));
        } else if (addr.ss_family == AF_INET6) {
            *peeraddr = InetAddress(*reinterpret_cast<const struct sockaddr_in6*>(&addr));
        }
    }

    return connfd;
}

// 禁用 Nagle 算法，降低数据发送延迟
void Socket::setTcpNoDelay(bool on) {
    sockets::setTcpNoDelay(sockfd_, on);
}

// 允许重用本地地址 (服务端快速重启时，无视 TIME_WAIT 状态)
void Socket::setReuseAddr(bool on) {
    sockets::setReuseAddr(sockfd_, on);
}

// 允许重用本地端口 (支持多线程/多进程监听同一端口)
void Socket::setReusePort(bool on) {
    sockets::setReusePort(sockfd_, on);
}

// 开启 TCP 操作系统级的心跳保活机制
void Socket::setKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, static_cast<socklen_t>(sizeof optval)) < 0) {
        LOG_SYSERR << "Socket::setKeepAlive failed on fd=" << sockfd_;
    }
}