#include "novanet/net/TcpServer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/Acceptor.h"
#include "novanet/net/EventLoopThreadPool.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/base/Logger.h"




#include <cstdio>
#include <cassert>

using namespace novanet::net;
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

// 内部帮助函数：提供默认的连接回调和消息回调
static void defaultConnectionCallback(const TcpConnectionPtr& conn) {
    LOG_INFO << "TcpServer::defaultConnectionCallback - " 
             << conn->localAddress().toIpPort() << " -> "
             << conn->peerAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");
}

static void defaultMessageCallback(const TcpConnectionPtr& conn ,Buffer* buf){
    buf->retrieveAll();// 默认丢弃所有数据
}


TcpServer::TcpServer(EventLoop* loop,
            const InetAddress& listenAddr,
            std::string nameArg):loop_(loop),
            ipPort_(listenAddr.toIpPort()),
            name_(std::move(nameArg)),
            acceptor_(std::make_unique<Acceptor>(loop,listenAddr,true)),// reuseport = true
            threadPool_(std::make_unique<EventLoopThreadPool>(loop)),
            connectionCallback_(defaultConnectionCallback),
            messageCallback_(defaultMessageCallback) {
    assert(loop != nullptr);

    //【核心组装】把 Acceptor 接入新连接的行为，绑定到 TcpServer::newConnection 上
    acceptor_->setNewConnectionCallback(
        [this](int sockfd,const InetAddress peerAddr){
            this->newConnection(sockfd, peerAddr);
        }
    );
    LOG_INFO << "TcpServer::TcpServer [" << name_ << "] constructed.";
}


TcpServer::~TcpServer(){
    loop_->assertInLoopThread();
    LOG_INFO << "TcpServer::~TcpServer [" << name_ << "] destructing";

    for(auto& item : connections_){
        TcpConnectionPtr conn(item.second);
        item.second.reset(); // 从 Map 的 value 里释放 shared_ptr


        // 必须让连接在它自己的 I/O 线程里销毁，避免死锁或时序问题
        conn->getLoop()->runInLoop([conn](){
            conn->connectDestroyed();
        });
    }
}

void TcpServer::setThreadNum(int numThreads){
    assert(0 <= numThreads);
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::start(){
    // 使用原子操作，保证 start() 被多次调用也是安全的
    if(started_.fetch_add(1) == 0){
        LOG_INFO << "TcpServer::start [" << name_ << "] starting thread pool...";
        threadPool_->start(threadInitCallback_);

        assert(!acceptor_->listening());
        // 将监听动作投递到 Main Loop 执行
        loop_->runInLoop([this]() {
            acceptor_->listen();
        });
    }
}

// 核心：新连接的诞生 (运行在 Main Loop)
void TcpServer::newConnection(int sockfd,const InetAddress& peerAddr){
    loop_->assertInLoopThread();


    //证明 Main Loop 揽客
    LOG_INFO << "[TEST-1] Main Loop (Thread ID: " << std::this_thread::get_id() 
             << ") accepted connection from " << peerAddr.toIpPort();

    EventLoop* ioLoop = threadPool_->getNextLoop();

    // 2. 生成全局唯一的连接名称
    char buf[64];
    snprintf(buf,sizeof buf,"-%s#%d",ipPort_.c_str(),nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO << "TcpServer::newConnection [" << name_
             << "] - new connection [" << connName
             << "] from " << peerAddr.toIpPort();

    // 3. 获取本地地址 (因为前面 accept 时只拿到了 peerAddr)
    InetAddress localAddr(sockets::getLocalAddr(sockfd));

    // 4. 创建极其关键的 TcpConnection 对象
    TcpConnectionPtr conn = std::make_shared<TcpConnection>(
                                ioLoop, connName, sockfd, localAddr, peerAddr);
    
    // 5. 将新连接加入 map 保命
    connections_[connName] = conn;

    // 6. 将 TcpServer 持有的业务层回调，下发给这根具体的 Connection
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    // 【生命周期闭环】把 closeCallback_ 绑定回 TcpServer 的 removeConnection
    conn->setCloseCallback([this](const TcpConnectionPtr& c) {
        this->removeConnection(c);
    });

    // 7. 【跨线程防弹衣】
    // 将 connectEstablished 的执行，安全地投递到新分配的 I/O 线程中去！
    // 这保证了一个连接的事件注册（enableReading）永远只在它归属的 loop 里发生。
    ioLoop->runInLoop([conn]() {
        conn->connectEstablished();
    });
}

// 核心：连接的消亡 (跨线程流转)
void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 跨线程！
    // 既然 connections_ 这个 map 归属于 Main Loop，那就必须回到 Main Loop 去修改它
    loop_->runInLoop([this, conn]() {
        this->removeConnectionInLoop(conn);
    });
}

// 此函数现在安全地运行在 Main Loop 中
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
             << "] - connection " << conn->name();

    // 1. 从 map 中抹除！这意味着如果业务层没持有，此时 conn 的引用计数为 1 (仅剩参数里的 conn 拷贝)
    size_t n = connections_.erase(conn->name());
    assert(n == 1);

    // 2. 将最终的毁灭动作 connectDestroyed，踢回给连接所属的 I/O 线程去收尾！
    // 使用 queueInLoop 而不是 runInLoop，避免在当前调用栈过深时发生生命周期错乱。
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn]() {
        conn->connectDestroyed();
    });
}