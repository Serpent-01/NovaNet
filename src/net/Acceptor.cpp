#include "novanet/net/Acceptor.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/Channel.h"
#include "novanet/net/InetAddress.h"
#include "novanet/net/SocketsOps.h"
#include "novanet/base/Logger.h"


#include <asm-generic/errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>


using namespace novanet::net;

Acceptor::Acceptor(EventLoop* loop,const InetAddress& listenAddr,bool reuseport)
    :loop_(loop),
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
    acceptChannel_(std::make_unique<Channel>(loop_,acceptSocket_.fd())),
    idleFd_(::open("/dev/null",O_RDONLY | O_CLOEXEC)) {
    
    assert(idleFd_ >= 0);
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reuseport);
    acceptSocket_.bindAddress(listenAddr);
    acceptChannel_->setReadCallback(std::bind(&Acceptor::handleRead,this));//
}

Acceptor::~Acceptor(){
    acceptChannel_->disableAll();
    acceptChannel_->remove();
    ::close(idleFd_);
}


void Acceptor::listen(){
    loop_->assertInLoopThread();
    listening_ = true;

    acceptSocket_.listen();

    acceptChannel_->enableReading();
    LOG_INFO << "Acceptor is listening";
}

void Acceptor::handleRead(){
    loop_->assertInLoopThread();
    InetAddress peerAddr;
    while(true){
        int connfd = acceptSocket_.accept(&peerAddr);
        if(connfd >=0){
            if(newConnectionCallback_){
                newConnectionCallback_(connfd,peerAddr);
            }else{
                sockets::close(connfd);
            }
        }else{
            int savedErrno = errno;

            if(savedErrno == EAGAIN || savedErrno == EWOULDBLOCK){
                break;
            }

            else if(savedErrno == EMFILE || savedErrno == ENFILE){
                LOG_ERROR << "Acceptor::handleRead EMFILE reached limit! No more fds.";

                ::close(idleFd_);
                int dropFd = acceptSocket_.accept(nullptr);

                if(dropFd >=0){
                    sockets::close(dropFd);
                } 
                idleFd_ = ::open("/dev/null",O_RDONLY | O_CLOEXEC);
                break;

            }else if(savedErrno == ECONNABORTED || savedErrno == EINTR || savedErrno == EPROTO || savedErrno == EPERM){
                LOG_WARN << "Acceptor::handleRead encountered transient error: " << savedErrno;
                continue;
            }else {
                LOG_SYSERR << "Acceptor::handleRead unknown transient error";
                break;
            }
        }
    }
}