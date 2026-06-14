#pragma once


#include "novanet/net/TcpConnection.h"

#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <functional>

namespace novanet::net{

class Acceptor;
class EventLoop;
class EventLoopThreadPool;

/**
 * @brief TCP 服务端类 (Phase 3 核心)
 * 
 * 架构定位：
 * - 扮演 Main Reactor 的角色，负责监听接入新连接。
 * - 拥有 EventLoopThreadPool，将新连接轮询分发给 Sub Reactors (I/O 线程)。
 * - 维护所有处于活跃状态的 TcpConnection 生命周期。
 */

class TcpServer{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    TcpServer(EventLoop* loop,const InetAddress& listenAddr,std::string nameArg);

    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    const std::string& ipPort() const {return ipPort_;}
    const std::string& name() const{return name_;}
    EventLoop* getLoop() const{return loop_;}

    // 线程池控制接口
    // 设置 sub loop 的线程数。必须在 start() 之前调用。
    // 0: 单线程模型 (所有 I/O 和 Accept 都在 Main Loop)
    // 1: 1 个 Main Loop (Accept) + 1 个 Sub Loop (I/O)
    // N: 1 个 Main Loop (Accept) + N 个 Sub Loop (I/O)
    void setThreadNum(int numThreads);
    void setThreadInitCallback(ThreadInitCallback cb){
        threadInitCallback_ = std::move(cb);
    }

    // 启动服务端，安全且只能调用一次
    void start();

    // 回调注册接口 (透传给 TcpConnection)
    void setConnectionCallback(TcpConnection::ConnectionCallback cb){
        connectionCallback_ = std::move(cb);
    }

    void setMessageCallback(TcpConnection::MessageCallback cb){
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(TcpConnection::WriteCompleteCallback cb){
        writeCompleteCallback_ = std::move(cb);
    }

private:
    // 供 Acceptor 调用的回调：当有新连接到达时
    void newConnection(int sockfd,const InetAddress& peerAddr);

    // 供 TcpConnection 调用的回调：当连接被断开时
    void removeConnection(const TcpConnection::TcpConnectionPtr& conn);

    // 确保连接从 map 中抹除的操作发生于 Main Loop 线程
    void removeConnectionInLoop(const TcpConnection::TcpConnectionPtr& conn);

    using ConnectionMap = std::unordered_map<std::string,TcpConnection::TcpConnectionPtr>;

    EventLoop* loop_; // Main Loop，负责 Acceptor

    const std::string ipPort_;
    const std::string name_;

    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;


    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    TcpConnection::WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_; 

    std::atomic_int32_t started_{0};// 防止多次 start()，使用 atomic 保证线程可见性
    int nextConnId_{1}; // 用于生成唯一连接名，仅在 Main Loop 中使用，无需加锁

    ConnectionMap connections_; // 必须仅在 Main Loop 中访问
};

}//namespace novanet::net