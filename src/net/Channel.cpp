#include "novanet/net/Channel.h"
#include "novanet/net/EventLoop.h"
#include <cassert>
#include <sys/epoll.h>

using namespace novanet::net;

const int Channel::kNoneEvent = 0;


const int Channel::kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop,int fd)
             : loop_(loop),
             fd_(fd),
             events_(kNoneEvent),
             revents_(0),
             index_(-1),
             eventHandling_(false) {
    assert(loop != nullptr);
}

Channel::~Channel(){
    assert(!eventHandling_);
}

void Channel::update(){
    loop_->updateChannel(this);
}

void Channel::remove(){
    assert(isNoneEvent);
    loop->removeChanne(this);
}

void Channel::handleEvent(){
    eventHandling_ = true;
    if((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)){
        if(closeCallback_){
            closeCallback_();
        }

        eventHandling_ = false;
        return;
    }

    if(revents_ & EPOLLERR){
        if(errorCallback_){
            errorCallback_();
        }
    }

    if(revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)){
        if(readCallback_){
            readCallback_();
        }
    }

    if(revents_ & EPOLLOUT){
        if(writeCallback_){
            writeCallback_();
        }
    }
    eventHandling_ = false;
}