#pragma once

#include <memory>
#include <string>
#include <functional>
#include <any>

#include "novanet/net/Buffer.h"
#include "novanet/net/InetAddress.h"

namespace novanet::net{
class EventLoop;
class Channel;
class Socket;

/**
 * @brief TCP 连接类 (Phase 3 核心)
 * 
 * 架构定位：
 * - 代表一个已建立的 TCP 连接。
 * - 归属于且仅归属于一个特定的 EventLoop (one loop per thread)。
 * - 持有 Socket(fd) 和 Channel(事件)，并管理收发缓冲 (Buffer)。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection>{
public:
    enum class State{
        kConnecting,
        kConnected,
        kDisconnecting,
        kDisConnected
    };
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;


    TcpConnection(EventLoop* loop,std::string name,
                int sockfd,const InetAddress& localAddr,const InetAddress& peerAddr);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* getLoop() const {return loop_;}
    const std::string& name() const {return name_;}
    const InetAddress& localAddress() const {return localAddr_; }
    const InetAddress& peerAddress() const {return peerAddr_;}

    bool connected() const {return state_ == State::kConnected; }
    bool disconnected() const{return state_ == State::kDisConnected; }


    // 跨线程安全接口 (可被其他业务线程调用)
    void send(const std::string& message);
    void send(const void* message, size_t len);
    void send(Buffer* message); // 经常用于转发数据
    void shutdown(); // 优雅半关闭
    void forceClose(); // 强制关闭


    // 上下文扩展 (C++17 std::any)
    void setContext(const std::any& context) { context_ = context; }
    const std::any& getContext() const { return context_; }
    std::any* getMutableContext() { return &context_; }
    

    // 回调注入
    void setConnectionCallback(ConnectionCallback cb) {connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) {messageCallback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, size_t highWaterMark) { 
        highWaterMarkCallback_ = std::move(cb); 
        highWaterMark_ = highWaterMark; 
    }

    // 生命周期与状态机流转 (仅限所属 EventLoop 线程调用)
    void connectEstablished();
    void connectDestroyed();
    



private:
    void setState(State s) {state_ = s;}

    //绑定到 Channel 的核心底层回调
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    // I/O 线程内真实的调用，使用你底层的 sockets:: API
    void sendInLoop(const void* data, size_t len);
    void sendInLoop(const std::string& message);
    void shutdownInLoop();
    void forceCloseInLoop();



    EventLoop* loop_;
    const std::string name_;

    State state_ {State::kConnecting};
    bool reading_{true};

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    InetAddress localAddr_;
    InetAddress peerAddr_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;


    std::any context_; //绑定业务层数据 (如 HttpContext, RpcSession)

    ConnectionCallback connectionCallback_;
    MessageCallback  messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_; // 通知上层 TcpServer 移除自己
    size_t highWaterMark_{64 * 1024 * 1024}; // 默认 64MB 高水位线
};
    
}