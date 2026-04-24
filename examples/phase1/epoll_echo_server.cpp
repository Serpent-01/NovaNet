#include "novanet/net/Socket.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/base/Logger.h"

#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cerrno>

using namespace novanet::net;

// TODO F4: 升级连接上下文，增加 writeIndex，彻底解决擦除带来的 O(n) 内存拷贝问题
struct ConnectionContext {
    int fd;
    std::string outBuffer;
    size_t writeIndex{0};  // 记录已经发送到了哪里
    bool writing{false};
};

// TODO F7: 将全局状态收拢到一个最小类中，为 Phase 2 面向对象化打基础
class Phase1Server {
public:
    Phase1Server(uint16_t port) 
        : listenSock_(sockets::createNonblockingOrDie(AF_INET)) 
    {
        // TODO F8: 预留容量，防止高并发接入时 unordered_map 疯狂 rehash 拖慢速度
        connections_.reserve(100000);

        sockets::ignoreSigPipe();
        
        idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (idleFd_ < 0) {
            LOG_SYSFATAL << "Failed to open idleFd!";
        }

        listenSock_.setReuseAddr(true);
        listenSock_.setReusePort(true);

        InetAddress listenAddr(port);
        listenSock_.bindAddress(listenAddr);
        listenSock_.listen();

        epollfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epollfd_ < 0) {
            LOG_SYSFATAL << "epoll_create1 failed";
        }

        updateEpoll(listenSock_.fd(), EPOLLIN | EPOLLET, EPOLL_CTL_ADD);
        LOG_INFO << "Phase1Server is listening on port " << port << " (ET Mode)";
    }

    ~Phase1Server() {
        if (epollfd_ >= 0) ::close(epollfd_);
        if (idleFd_ >= 0) ::close(idleFd_);
    }

    void start() {
        std::vector<struct epoll_event> events(128);

        while (true) {
            int numEvents = ::epoll_wait(epollfd_, events.data(), static_cast<int>(events.size()), -1);
            
            if (numEvents < 0) {
                if (errno == EINTR) continue;
                LOG_SYSFATAL << "epoll_wait failed";
            }

            if (numEvents == static_cast<int>(events.size())) {
                events.resize(events.size() * 2);
            }

            for (int i = 0; i < numEvents; i++) {
                int currentFd = events[i].data.fd;
                uint32_t currentEvents = events[i].events;

                if (currentFd == listenSock_.fd()) {
                    handleNewConnection();
                } else {
                    auto it = connections_.find(currentFd);
                    if (it == connections_.end()) continue;
                    ConnectionContext& ctx = it->second;

                    // TODO F6: 不再一见 EPOLLERR/EPOLLHUP 就立刻 close。
                    // 而是优先让 EPOLLIN 去 read，利用 read 返回 0 来稳健地确认对端关闭，防止丢弃接收区残余数据。
                    if (currentEvents & (EPOLLIN | EPOLLRDHUP)) {
                        handleRead(currentFd, ctx);
                    }
                    
                    // 注意：如果 handleRead 中触发了 close，ctx 会失效，这里需要防范
                    // 但由于在 handleRead 失败时我们会从 connections_ 中 erase，
                    // 为了安全起见，再次 find 检查连接是否存活
                    if (connections_.find(currentFd) != connections_.end() && (currentEvents & EPOLLOUT)) {
                        handleWrite(currentFd, ctx);
                    }

                    // 兜底：如果既不可读又不可写，但挂了错误标志，强杀
                    if (connections_.find(currentFd) != connections_.end() && 
                        (currentEvents & (EPOLLERR | EPOLLHUP))) {
                        closeConnection(currentFd);
                    }
                }
            }
        }
    }

private:
    int epollfd_;
    int idleFd_;
    Socket listenSock_;
    std::unordered_map<int, ConnectionContext> connections_;

    // TODO F10: 消除魔法数字，将读缓冲提为常量
    static constexpr size_t kReadBufferSize = 64 * 1024;

    void updateEpoll(int fd, uint32_t events, int op) {
        struct epoll_event ev{};    
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epollfd_, op, fd, &ev) < 0) {
            LOG_SYSERR << "epoll_ctl error on fd " << fd;
        }
    }

    void closeConnection(int fd) {
        updateEpoll(fd, 0, EPOLL_CTL_DEL);
        connections_.erase(fd);
        sockets::close(fd);
        // TODO F5: 高频路径日志已降级为 INFO，在 Logger 的默认 Warn 级别下会被安全短路，不会拖慢压测
        LOG_INFO << "Connection closed: fd=" << fd;
    }

    void handleNewConnection() {
        while (true) {
            InetAddress peerAddr;
            int connfd = listenSock_.accept(&peerAddr); 
            if (connfd >= 0) {
                LOG_INFO << "New connection from " << peerAddr.toIpPort() << " accepted, fd=" << connfd;
                sockets::setTcpNoDelay(connfd, true);
                connections_[connfd] = ConnectionContext{connfd, "", 0, false};
                updateEpoll(connfd, EPOLLIN | EPOLLET, EPOLL_CTL_ADD);
            } else {
                // TODO F1: 修正 accept 错误分类，符合 ET 模式吃尽 backlog 的契约
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
                    // 暂态错误，不影响后面的新连接，继续 accept
                    continue;
                } else if (errno == EMFILE || errno == ENFILE) {
                    LOG_ERROR << "EMFILE reached! Dropping connection via idleFd...";
                    ::close(idleFd_);
                    struct sockaddr_storage dummy{};
                    int dropFd = sockets::accept(listenSock_.fd(), &dummy);
                    if (dropFd >= 0) {
                        sockets::close(dropFd);
                    }
                    idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                    // TODO F9: 闭环检查 reopen 是否成功，防止防御机制彻底失效
                    if (idleFd_ < 0) {
                        LOG_SYSFATAL << "Failed to reopen idleFd, protection lost!";
                        break;
                    }
                    continue;
                } else {
                    LOG_SYSERR << "Fatal accept error";
                    break;
                }
            }
        }
    }

    void handleRead(int fd, ConnectionContext& ctx) {
        char buf[kReadBufferSize];
        bool readError = false;

        while (true) {
            ssize_t n = sockets::read(fd, buf, sizeof(buf));
            if (n > 0) {
                // 如果当前没有积压的 outBuffer，尝试直接写入网卡，做最极限的短路优化
                if (ctx.outBuffer.empty()) {
                    ssize_t nwrote = sockets::write(fd, buf, n);
                    if (nwrote >= 0) {
                        if (static_cast<size_t>(nwrote) < static_cast<size_t>(n)) {
                            // 没写完，剩下的进 Buffer
                            ctx.outBuffer.append(buf + nwrote, n - nwrote);
                            ctx.writeIndex = 0; // 新增积压，从 0 开始记录
                        }
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            ctx.outBuffer.append(buf, n);
                            ctx.writeIndex = 0;
                        } else {
                            LOG_SYSERR << "Write error on fd=" << fd;
                            readError = true;
                            break;
                        }
                    }
                } else {
                    // 已经有积压了，直接追加，防止乱序
                    ctx.outBuffer.append(buf, n);
                }

                // 一旦有积压，注册 EPOLLOUT
                if (!ctx.outBuffer.empty() && !ctx.writing) {
                    updateEpoll(fd, EPOLLIN | EPOLLOUT | EPOLLET, EPOLL_CTL_MOD);
                    ctx.writing = true;
                }
            } else if (n == 0) {
                readError = true; // 对端正常关闭
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    LOG_SYSERR << "Read error on fd=" << fd;
                    readError = true;
                    break;
                }
            }
        }

        if (readError) {
            closeConnection(fd);
        }
    }

    void handleWrite(int fd, ConnectionContext& ctx) {
        // TODO F2 & F3: ET 模式核心准则——不断 Write 直到 EAGAIN 或写空。
        // 同时利用 writeIndex，彻底消除 string::erase(0, n) 带来的内存拷贝。
        while (ctx.writeIndex < ctx.outBuffer.size()) {
            const char* dataPtr = ctx.outBuffer.data() + ctx.writeIndex;
            size_t remainSize = ctx.outBuffer.size() - ctx.writeIndex;
            
            ssize_t n = sockets::write(fd, const_cast<char*>(dataPtr), remainSize);
            
            if (n > 0) {
                ctx.writeIndex += n; // 向前推进指针，完美零拷贝
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 内核缓冲区再次写满，停手，等待下次 EPOLLOUT
                } else {
                    LOG_SYSERR << "Write (flush) error on fd=" << fd;
                    closeConnection(fd);
                    return;
                }
            }
        }

        // 如果积压数据终于发完了，清理 Buffer 并关闭 EPOLLOUT 关注
        if (ctx.writeIndex == ctx.outBuffer.size()) {
            ctx.outBuffer.clear();
            ctx.writeIndex = 0;
            if (ctx.writing) {
                updateEpoll(fd, EPOLLIN | EPOLLET, EPOLL_CTL_MOD);
                ctx.writing = false;
            }
        }
    }
};

int main() {
    // 实例化 Phase1 终极服务器并启动主循环
    Phase1Server server(8080);
    server.start();
    return 0;
}