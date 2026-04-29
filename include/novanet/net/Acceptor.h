#pragma once


#include <functional>
#include <memory>
#include "novanet/net/Socket.h"
#include "novanet/net/InetAddress.h"

namespace novanet::net{
class EventLoop;
class Channel;

class Acceptor{
public:
    using NewConnectionCallback = std::function<void(int sockfd,const InetAddress&)>;
    
    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport = true);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb){
        newConnectionCallback_ = std::move(cb);
    }

    void listen();

    bool listening() const { return listening_; }

private:
    void handleRead();

private:
    EventLoop* loop_;
    
    Socket acceptSocket_;

    std::unique_ptr<Channel> acceptChannel_;

    bool listening_{false};

    int idleFd_;

    NewConnectionCallback newConnectionCallback_;
};

}