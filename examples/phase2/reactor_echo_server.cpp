#include "novanet/net/EventLoop.h"
#include "novanet/net/Acceptor.h"
#include "novanet/net/Channel.h"
#include "novanet/net/Buffer.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/base/Logger.h"

#include <unordered_map>
#include <memory>
#include <string>
#include <unistd.h>

using namespace novanet;
using namespace novanet::net;

struct Session{
    int sockfd;
    std::unique_ptr<Channel> channel;
    Buffer inputBuffer;
    Buffer outputBuffer;

    Session(EventLoop* loop ,int fd):sockfd(fd),channel(std::make_unique<Channel>(loop,fd)){}
    ~Session(){
        channel->disableAll();
        channel->remove();
        sockets::close(sockfd);
        LOG_INFO << "Session destructed, closed fd=" << sockfd;
    }
};

class EchoServer{
public:
    EchoServer(EventLoop* loop,const InetAddress& listenAddr)
        :loop_(loop),
        acceptor_(loop, listenAddr,true){
        
        acceptor_.setNewConnectionCallback(
            std::bind(&EchoServer::onNewConnection, this, std::placeholders::_1, std::placeholders::_2));
    }
    

    void start(){
        acceptor_.listen();
        LOG_INFO << "EchoServer started.";
    }
private:
    void onNewConnection(int sockfd,const InetAddress& peerAddr){
        LOG_INFO << "New connection accepted: fd=" << sockfd << " from " << peerAddr.toIpPort();
        auto session = std::make_unique<Session>(loop_, sockfd);
        // 2. 将事件回调绑定到该连接的 Channel 上
        session->channel->setReadCallback(std::bind(&EchoServer::onMessage, this, sockfd));
        session->channel->setWriteCallback(std::bind(&EchoServer::onWrite, this, sockfd));
        session->channel->setCloseCallback(std::bind(&EchoServer::onClose, this, sockfd));
        session->channel->setErrorCallback(std::bind(&EchoServer::onError, this, sockfd));
        // 3. 开始监听可读事件
        session->channel->enableReading();
        // 4. 将 Session 保存到哈希表中，维持生命周期
        sessions_[sockfd] = std::move(session);
    }

    // void onMessage(int sockfd){
    //     auto it = sessions_.find(sockfd);
    //     if(it == sessions_.end()) return;
    //     Session* session = it->second.get();

    //     int savedErrno = 0;

    //     ssize_t n = session->inputBuffer.readFd(sockfd, &savedErrno);
    //     if(n > 0){
    //         std::string msg = session->inputBuffer.retrieveAllAsString();
    //         LOG_INFO << "Received " << n << " bytes from fd=" << sockfd << ": " << msg;
    //         // 业务处理完毕，准备发送数据 (Echo 逻辑)
    //         session->outputBuffer.append(msg);
    //         // 尝试将 outputBuffer 发送出去
    //         sendData(session);
    //     }else if(n == 0){
    //         onClose(sockfd);
    //     }else {
    //         // 读错误
    //         LOG_SYSERR << "EchoServer::onMessage read error on fd=" << sockfd;
    //         onClose(sockfd);
    //     }
    // }

    // void onMessage(int sockfd) {
    //     auto it = sessions_.find(sockfd);
    //     if (it == sessions_.end()) return;
    //     Session* session = it->second.get();

    //     int savedErrno = 0;
        
    //     // 【修改点】：直接去掉 while(true)，读一次就行了！
    //     ssize_t n = session->inputBuffer.readFd(sockfd, &savedErrno);

    //     if (n > 0) {
    //         size_t readable = session->inputBuffer.readableBytes();
    //         session->outputBuffer.append(session->inputBuffer.peek(), readable);
    //         session->inputBuffer.retrieve(readable);
    //     } 
    //     else if (n == 0) {
    //         onClose(sockfd);
    //         return; 
    //     } 
    //     else {
    //         // 在 LT 模式下，单次 read 偶尔也会报 EAGAIN，直接忽略，不当作错误
    //         if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
    //             LOG_SYSERR << "EchoServer::onMessage read error on fd=" << sockfd;
    //             onClose(sockfd);
    //         }
    //         return;
    //     }
        
    //     sendData(session);
    // }
    // void onMessage(int sockfd) {
    //     auto it = sessions_.find(sockfd);
    //     if (it == sessions_.end()) return;
    //     Session* session = it->second.get();

    //     int savedErrno = 0;
        
    //     // 【王者归来】：恢复激进读取 (Aggressive Read)，榨干单次 epoll 唤醒的价值！
    //     while (true) {
    //         ssize_t n = session->inputBuffer.readFd(sockfd, &savedErrno);

    //         if (n > 0) {
    //             size_t readable = session->inputBuffer.readableBytes();
    //             session->outputBuffer.append(session->inputBuffer.peek(), readable);
    //             session->inputBuffer.retrieve(readable);
                
    //             // 优化：如果你一次性读到了极其巨大的数据（比如大于 64KB）
    //             // 为了防止饿死其他连接，你可以选择 break，把剩下的交给 LT 模式下次处理。
    //             // 但对于 Ping-Pong 小包，它会一直读到 EAGAIN。
    //         } 
    //         else if (n == 0) {
    //             onClose(sockfd);
    //             return; 
    //         } 
    //         else {
    //             if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
    //                 // 读干净了！完美退出循环。
    //                 break; 
    //             }
    //             LOG_SYSERR << "EchoServer::onMessage read error on fd=" << sockfd;
    //             onClose(sockfd);
    //             return;
    //         }
    //     }
        
    //     // 集中发送刚才掏出来的所有数据
    //     sendData(session);
    // }


    void onMessage(int sockfd) {
        auto it = sessions_.find(sockfd);
        if (it == sessions_.end()) return;
        Session* session = it->second.get();

        int savedErrno = 0;
        
        // 【纯正 LT 模式标准写法】：去掉 while(true)，读一次就行了！
        ssize_t n = session->inputBuffer.readFd(sockfd, &savedErrno);

        if (n > 0) {
            size_t readable = session->inputBuffer.readableBytes();
            session->outputBuffer.append(session->inputBuffer.peek(), readable);
            session->inputBuffer.retrieve(readable);
        } 
        else if (n == 0) {
            onClose(sockfd);
            return; 
        } 
        else {
            if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
                LOG_SYSERR << "EchoServer::onMessage read error on fd=" << sockfd;
                onClose(sockfd);
            }
            return;
        }
        
        sendData(session);
    }

    void sendData(Session* session) {
        size_t readable = session->outputBuffer.readableBytes();
        if (readable == 0) return;

        ssize_t nwrote = 0;
        
        // 【核心法则】：如果当前没有在排队等发送（没有开启 EPOLLOUT）
        // 我们才尝试直接抢发。如果已经开启了，说明内核满了，直接跳过 write！
        if (!session->channel->isWriting()) {
            nwrote = sockets::write(session->sockfd, session->outputBuffer.peek(), readable);
            
            if (nwrote >= 0) {
                session->outputBuffer.retrieve(nwrote);
                if (session->outputBuffer.readableBytes() == 0) {
                    // 全发完了，太棒了，啥都不用做
                    LOG_INFO << "Send all data directly on fd=" << session->sockfd;
                    return; 
                }
            } else {
                nwrote = 0;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_SYSERR << "EchoServer::sendData write error";
                    return;
                }
            }
        }

        // 如果走到了这里，说明有两种情况：
        // 1. 刚才抢发了，但没发完（被 EAGAIN 挡住了）
        // 2. 压根没抢发（因为早就 isWriting() 了，直接把数据堆在 Buffer 里了）
        // 
        // 无论哪种情况，只要 Buffer 里还有剩余数据，且还没监听 EPOLLOUT，就必须监听！
        if (session->outputBuffer.readableBytes() > 0 && !session->channel->isWriting()) {
            LOG_INFO << "Cannot send all data, enable EPOLLOUT for fd=" << session->sockfd;
            session->channel->enableWriting();
        }
    }

    void onWrite(int sockfd) {
        auto it = sessions_.find(sockfd);
        if (it == sessions_.end()) return;
        Session* session = it->second.get();

        if (session->channel->isWriting()) {
            size_t readable = session->outputBuffer.readableBytes();
            ssize_t n = sockets::write(sockfd, session->outputBuffer.peek(), readable);
            
            if (n > 0) {
                session->outputBuffer.retrieve(n);
                
                // 如果这次把欠下的债全还清了
                if (session->outputBuffer.readableBytes() == 0) {
                    // 立刻关闭 EPOLLOUT
                    session->channel->disableWriting();
                    LOG_INFO << "Finished sending remaining data, disabled EPOLLOUT for fd=" << sockfd;
                } 
                // 如果 n > 0 但还没发完，说明内核又满了。
                // 此时什么都不用做！因为 EPOLLOUT 还没关，下次内核有空还会唤醒我们！
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_SYSERR << "EchoServer::onWrite error";
                    onClose(sockfd);
                }
            }
        }
    }
    // void onClose(int sockfd) {
    //     LOG_INFO << "Connection closed, preparing to remove fd=" << sockfd;
        
    //     // 【核心架构修复：延迟销毁 (Delayed Destruction)】
    //     // 绝对不能在这里直接 sessions_.erase(sockfd)！
    //     // 因为当前的调用栈还在 Channel::handleEvent() 内部。
    //     // 我们利用 EventLoop 的异步任务队列，把销毁动作推迟到本轮 epoll 分发结束之后执行。
    //     loop_->queueInLoop([this, sockfd]() {
    //         sessions_.erase(sockfd);
    //         LOG_INFO << "Deferred destruction completed for fd=" << sockfd;
    //     });
    // }
    void onClose(int sockfd) {
        // 1. 第一层防御：哈希表级别防重删
        auto it = sessions_.find(sockfd);
        if (it == sessions_.end()) {
            return; // 已经被彻底清理了，直接忽略
        }

        Session* session = it->second.get();

        // 2. 第二层防御（核心护盾）：状态机级别防 Double Close
        // 如果这个 Channel 已经注销了所有事件，说明它已经在“等死”队列里了
        // 即使同一个 handleEvent 周期里又报了错，也会在这里被完美挡下！
        if (session->channel->isNoneEvent()) {
            return;
        }

        LOG_INFO << "Connection closed, preparing to remove fd=" << sockfd;

        // 3. 拔掉呼吸机：立刻把它从 epoll 监听树上彻底摘除！
        // 这一步极其关键，确保它在真正销毁前，绝不会再产生任何新事件
        session->channel->disableAll();

        // 4. 推入太平间：交给 EventLoop 的异步队列，在本次 epoll 循环结束后安全回收内存
        loop_->queueInLoop([this, sockfd]() {
            sessions_.erase(sockfd);
            LOG_INFO << "Deferred destruction completed for fd=" << sockfd;
        });
    }

    void onError(int sockfd) {
        LOG_ERROR << "Error occurred on fd=" << sockfd;
        onClose(sockfd);
    }
private:
    EventLoop* loop_;
    Acceptor acceptor_;
    
    // 连接管理容器：通过 fd 映射其专属的 Session
    std::unordered_map<int, std::unique_ptr<Session>> sessions_;
};


int main() {
    // 强制设定日志级别，测试阶段可设为 Info 查看全链路轨迹
    //novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Error);
    LOG_INFO << "Starting NovaNet Phase 2 Reactor Echo Server...";

    // 1. 初始化 Reactor 的核心：EventLoop
    EventLoop loop;

    // 2. 初始化服务端地址，监听 8080 端口，绑定所有网卡 (INADDR_ANY)
    InetAddress listenAddr(8080, false, false);

    // 3. 初始化并启动 EchoServer
    EchoServer server(&loop, listenAddr);
    server.start();

    // 4. 发动引擎，死循环跑起来！
    loop.loop();

    return 0;
}