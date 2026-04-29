#include "novanet/net/Poller.h"
#include "novanet/net/Channel.h"
#include "novanet/base/Logger.h"
#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <cassert>

using namespace novanet::net;
Poller::Poller(EventLoop* loop):ownerLoop_(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
    events_(kInitEventListSize) {
    assert(epollfd_>=0);
}

Poller::~Poller(){
    ::close(epollfd_);
}

void Poller::poll(int timeoutMs,ChannelList* activeChannels){
    int numsEvents = ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
    int savedErrno = errno;

    if(numsEvents > 0){
        fillActiveChannels(numsEvents, activeChannels);
        if(static_cast<size_t>(numsEvents) == events_.size()){
            events_.resize(events_.size()*2);
        }
    }else if(numsEvents == 0){
        // 仅仅是超时，没有任何网络事件
    }else{
        if(savedErrno != EINTR){
            LOG_ERROR << "epoll_wait WA errcode= " << errno; 
        }
    }


    void Poller::fillActiveChannels(int numsEvents,ChannelList* activeChannels) const{
        assert(static_cast<size_t>(numsEvents) <= events_.size());
        for(int i = 0;i<numsEvents;i++){
            //在注册epoll_ctl的时候，我们将channel对象，塞进了 epoll_event.data.ptr里
            //events_[i].data.ptr 的类型是 void*（C 语言风格的类型擦除）。
            // 编译器只知道它是一个内存地址，但不知道这个地址上存的是什么类型的数据。
            Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
            channel->set_revents(events_[i].events);
            activeChannels->push_back(channel);
        }
    }


}