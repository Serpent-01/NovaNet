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

    //卸下沙袋：
    void onMessage(int sockfd) {
        auto it = sessions_.find(sockfd);
        if (it == sessions_.end()) return;
        Session* session = it->second.get();

        int savedErrno = 0;
        ssize_t n = session->inputBuffer.readFd(sockfd, &savedErrno);

        if (n > 0) {
            // 【极限优化】：不要转成 std::string！直接把输入 Buffer 的数据移交进输出 Buffer
            size_t readable = session->inputBuffer.readableBytes();
            
            // 直接读取底层的 const char* 进行 append，不产生任何临时对象
            session->outputBuffer.append(session->inputBuffer.peek(), readable);
            
            // 消费掉输入缓冲区的游标
            session->inputBuffer.retrieve(readable);
            
            // 发送数据
            sendData(session);
        } 
        else if (n == 0) {
            onClose(sockfd);
        } 
        else {
            onClose(sockfd);
        }
    }


    // 满足需求 6.3: 发送数据的流 (重点：写不完时保留剩余数据，打开 EPOLLOUT)
    void sendData(Session* session){
        size_t readable = session->outputBuffer.readableBytes();
        if(readable == 0) return;

        ssize_t nwrote = 0;
        if(!session->channel->isWriting()){
            nwrote = sockets::write(session->sockfd, session->outputBuffer.peek(), readable);
            if(nwrote >=0){
                session->outputBuffer.retrieve(nwrote);
                // 如果一次性全写完了，完美！
                if (session->outputBuffer.readableBytes() == 0) {
                    LOG_INFO << "Send all data directly on fd=" << session->sockfd;
                }
            }else{
                nwrote = 0;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_SYSERR << "EchoServer::sendData write error";
                }
            }
        }
        // 核心精髓：如果没写完 (nwrote < readable)，或者因为 EAGAIN 没写，
        // 必须开启 EPOLLOUT，让 epoll 下次可写时通知我们！
        if (session->outputBuffer.readableBytes() > 0 && !session->channel->isWriting()) {
            LOG_INFO << "Cannot send all data, enable EPOLLOUT for fd=" << session->sockfd;
            session->channel->enableWriting();
        }
    }

    void onWrite(int sockfd){
        auto it = sessions_.find(sockfd);
        if (it == sessions_.end()) return;
        Session* session = it->second.get();
        if(session->channel->isWriting()){
            ssize_t n = sockets::write(sockfd, 
                session->outputBuffer.peek(),
                 session->outputBuffer.readableBytes());
            
            if(n > 0){
                session->outputBuffer.retrieve(n);
                // 如果这次终于把数据全写完了，必须立刻关闭 EPOLLOUT 事件，
                // 否则 epoll 的 LT 模式会疯狂唤醒你，导致 CPU 100% 飙升！
                if (session->outputBuffer.readableBytes() == 0) {
                    session->channel->disableWriting();
                    LOG_INFO << "Finished sending remaining data, disabled EPOLLOUT for fd=" << sockfd;
                } 
            } else {
                LOG_SYSERR << "EchoServer::onWrite error";
            }
        }
    }
    void onClose(int sockfd) {
        LOG_INFO << "Connection closed, preparing to remove fd=" << sockfd;
        
        // 【核心架构修复：延迟销毁 (Delayed Destruction)】
        // 绝对不能在这里直接 sessions_.erase(sockfd)！
        // 因为当前的调用栈还在 Channel::handleEvent() 内部。
        // 我们利用 EventLoop 的异步任务队列，把销毁动作推迟到本轮 epoll 分发结束之后执行。
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
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);
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