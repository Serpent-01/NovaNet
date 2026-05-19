#include "novanet/net/TcpConnection.h"

#include <unistd.h>

#include <cassert>

#include "novanet/base/Logger.h"  // 宏级短路日志
#include "novanet/net/Channel.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/Socket.h"
#include "novanet/net/SocketsOps.h"

using namespace novanet::net;

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop),
      name_(std::move(name)),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr) {
    // 将自身的处理逻辑绑定到底层 Channel
    channel_->setReadCallback([this]() { handleRead(); });
    channel_->setWriteCallback([this]() { handleWrite(); });
    channel_->setCloseCallback([this]() { handleClose(); });
    channel_->setErrorCallback([this]() { handleError(); });

    LOG_INFO << "TcpConnection::ctor[" << name_ << "] at fd=" << sockfd;
}

TcpConnection::~TcpConnection() {
    LOG_INFO << "TcpConnection::dtor[" << name_ << "] at fd=" << channel_->fd()
             << " state=" << static_cast<int>(state_);
    assert(state_ == State::kDisConnected);
}

// 跨线程安全接口：send 与 shutdown
void TcpConnection::send(const std::string& message) {
    if (state_ == State::kConnected) {
        if (loop_->isInLoopThread()) {
            // 如果已经在所属的 I/O 线程，直接发送以获得极速性能
            sendInLoop(message.data(), message.size());  //零拷贝极速路径
        } else {
            // 【跨线程核心！】
            // 如果是业务线程调用，将发送任务投递回专属 I/O 线程
            // 注意：必须按值捕获 message (复制数据)，并通过 shared_from_this
            // 延长生命周期
            loop_->runInLoop([conn = shared_from_this(), msg = message]() {
                conn->sendInLoop(msg.data(), msg.size());
            });
        }
    }
}

void TcpConnection::send(const void* data, size_t len) {
    if (state_ == State::kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(data, len);
        } else {
            // 极其危险的裸指针跨线程，必须转为 string 进行深拷贝续命
            std::string message(static_cast<const char*>(data), len);
            loop_->runInLoop(
                [conn = shared_from_this(), msg = std::move(message)]() {
                    conn->sendInLoop(msg.data(), msg.size());
                });
        }
    }
}

void TcpConnection::send(Buffer* buf) {
    if (state_ == State::kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
        } else {
            std::string message(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
            loop_->runInLoop(
                [conn = shared_from_this(), msg = std::move(message)]() {
                    conn->sendInLoop(msg.data(), msg.size());
                });
        }
    }
}

// 核心 I/O 逻辑 (严格限制在 EventLoop 所属线程)
void TcpConnection::sendInLoop(const std::string& message) {
    sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    loop_->assertInLoopThread();
    ssize_t nwrote = 0;
    size_t remaining = len;

    bool faultError = false;

    if (state_ == State::kDisConnected) {
        LOG_WARN << "disconnected, give up writing";
        return;
    }

    // 尝试直接使用底层 sockets::write 突破内核缓冲区
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = sockets::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                loop_->queueInLoop([conn = shared_from_this()]() {
                    conn->writeCompleteCallback_(conn);
                });
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                LOG_SYSERR << "TcpConnection::sendInLoop write error";
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }
    // 处理积压
    assert(remaining <= len);
    if (!faultError && remaining > 0) {
        //必测 7：output buffer 正确
        // 【新增探针 1】：记录背压（Backpressure）发生，内核发送窗口塞满
        LOG_INFO << "[Backpressure] 发送窗口已满！尝试发送 " << len
                 << " 字节，实际只发了 " << nwrote << " 字节。剩余 "
                 << remaining << " 字节追加到 OutputBuffer，并注册 EPOLLOUT！";

        size_t oldLen = outputBuffer_.readableBytes();

        // 高水位回调触发保护机制
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ &&
            highWaterMarkCallback_) {
            loop_->queueInLoop(
                [conn = shared_from_this(), len = oldLen + remaining]() {
                    conn->highWaterMarkCallback_(conn, len);
                });
        }

        outputBuffer_.append(static_cast<const char*>(data) + nwrote,
                             remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdown() {
    if (state_ == State::kConnected) {
        setState(State::kDisconnecting);
        loop_->runInLoop(
            [conn = shared_from_this()]() { conn->shutdownInLoop(); });
    }
}

void TcpConnection::forceClose() {
    if (state_ == State::kConnected || state_ == State::kDisconnecting) {
        setState(State::kDisconnecting);
        loop_->queueInLoop(
            [conn = shared_from_this()]() { conn->forceCloseInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        socket_->shutdownWrite();
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kConnected || state_ == State::kDisconnecting) {
        handleClose();
    }
}

//事件回调 (由 Channel 触发)
void TcpConnection::handleRead() {
    loop_->assertInLoopThread();

    int savedErrno = 0;

    // 使用 muduo 标准的 readFd 解决分散读问题
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        messageCallback_(shared_from_this(), &inputBuffer_);
    } else if (n == 0) {
        handleClose();
    } else {
        errno = savedErrno;
        LOG_SYSERR << "TcpConnection::handleRead";
        handleError();
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();

    // LOG_INFO << "[TEST-1] Sub Loop (Thread ID: " <<
    // std::this_thread::get_id()
    //          << ") handling WRITE for " << name_;

    if (channel_->isWriting()) {
        ssize_t n = sockets::write(channel_->fd(), outputBuffer_.peek(),
                                   outputBuffer_.readableBytes());
        if (n > 0) {
            outputBuffer_.retrieve(n);

            //必测 7：output buffer 正确
            // 【新增探针 2】：记录 EPOLLOUT 的局部消耗
            LOG_INFO
                << "[handleWrite] EPOLLOUT 触发，成功消耗 OutputBuffer 里的 "
                << n << " 字节。";

            if (outputBuffer_.readableBytes() == 0) {
                channel_->disableWriting();

                //必测 7：output buffer 正确
                // 【新增探针 3】：记录彻底清空，防止 Busy Loop
                LOG_INFO << "[handleWrite] OutputBuffer 已全部清空，注销 "
                            "EPOLLOUT 关注，防止 Busy Loop！";

                if (writeCompleteCallback_) {
                    loop_->queueInLoop([conn = shared_from_this()]() {
                        conn->writeCompleteCallback_(conn);
                    });
                }
                if (state_ == State::kDisconnecting) {
                    shutdownInLoop();
                }
            }
        } else {
            LOG_SYSERR << "TcpConnection::handleWrite";
        }
    } else {
        LOG_WARN << "Connection fd = " << channel_->fd()
                 << " is down, no more writing";
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    LOG_INFO << "TcpConnection::handleClose fd=" << channel_->fd()
             << " state=" << static_cast<int>(state_);
    assert(state_ == State::kConnected || state_ == State::kDisconnecting);

    setState(State::kDisConnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(
        shared_from_this());  // 提升引用计数防止在此函数内被意外销毁

    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    if (closeCallback_) {
        closeCallback_(guardThis);  // 通知 TcpServer 从容器中移除
    }
}

void TcpConnection::handleError() {
    int err = sockets::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection::handleError [" << name_
              << "] - SO_ERROR = " << err;
}

// 生命周期控制 (由 TcpServer 调用)
void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    assert(state_ == State::kConnecting);
    setState(State::kConnected);

    // 【重要！】防弹衣合拢：将自身 shared_ptr 的弱引用交给 Channel
    // 确保 Channel 执行事件时，自己绝对不会被销毁
    channel_->tie(shared_from_this());

    // 向 epoll 注册可读事件
    channel_->enableReading();

    // 触发用户侧的连接建立回调
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();

    // 【就是这句确切的日志代码】： test8
    LOG_INFO << "TcpConnection::connectDestroyed [" << name_
             << "] fd=" << channel_->fd()
             << " state=" << static_cast<int>(state_);

    if (state_ == State::kConnected) {
        setState(State::kDisconnecting);
        channel_->disableAll();  // 停止向 epoll 订阅任何事件

        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    // 把 Channel 从 EventLoop 中彻底注销
    channel_->remove();
}
