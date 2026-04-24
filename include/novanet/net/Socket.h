#pragma once

namespace novanet::net {

class InetAddress;

class Socket {
public:
    explicit Socket(int sockfd) noexcept : sockfd_(sockfd) {}

    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    
    // 2. 开启移动语义！允许 Socket 被放入 std::vector 等容器中流转
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int fd() const { return sockfd_; }

    void bindAddress(const InetAddress& address);

    void listen();

    [[nodiscard]] int accept(InetAddress* peeraddr);

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

private:
    int sockfd_;
};

}  // namespace novanet::net