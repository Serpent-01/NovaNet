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
    if(epollfd_ < 0){
        LOG_SYSFATAL << "Poller::epoll_create1 failed!";
    }
}

Poller::~Poller(){
    ::close(epollfd_);
}

void Poller::poll(int timeoutMs,ChannelList* activeChannels){
    int numEvents = ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
    int savedErrno = errno;

    if(numEvents > 0){
        // 【日志接入】: 调试热路径，只有在设置了 INFO 级别时才会打印，零开销
        LOG_INFO << numEvents << " events happened";
        fillActiveChannels(numEvents, activeChannels);
        if(static_cast<size_t>(numEvents) == events_.size()){
            events_.resize(events_.size()*2);
        }
    }else if(numEvents == 0){
        // 仅仅是超时，没有任何网络事件
    }else{
        if(savedErrno != EINTR){
            // 【日志接入】: 捕捉除了被信号中断(EINTR)之外的所有底层 epoll 异常
            errno = savedErrno; // 恢复 errno 供 LOG_SYSERR 内部读取
            LOG_SYSERR << "Poller::poll() failed!";
        }
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



void Poller::updateChannel(Channel* channel){
    const int index = channel->index();
    if(index == -1 || index == 2){
        int fd = channel->fd();
        if(index == -1){
            assert(channels_.find(fd) == channels_.end());
            channels_[fd] = channel;
        }else{
            assert(channels_.find(fd) != channels_.end());
            assert(channels_[fd] == channel);
        }
        channel->set_index(1);
        update(EPOLL_CTL_ADD,channel);
    }else{
        int fd = channel->fd();
        assert(channels_.find(fd) != channels_.end());
        assert(channels_[fd] == channel);
        assert(index == 1);

        if(channel->isNoneEvent()){
            update(EPOLL_CTL_DEL,channel);
            channel->set_index(2);
        }else{
            update(EPOLL_CTL_MOD,channel);
        }
    }
}


void Poller::removeChannel(Channel* channel){
    int fd = channel->fd();
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(channel->isNoneEvent());
    
    int index = channel->index();
    assert(index == 1 || index == 2);

    size_t n = channels_.erase(fd);
    assert(n == 1);
    if(index == 1){
        update(EPOLL_CTL_DEL,channel);
    }
    channel->set_index(-1);
}

bool Poller::hasChannel(Channel* channel) const{
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

void Poller::update(int operation,Channel* channel){
    struct epoll_event ev;
    std::memset(&ev,0,sizeof(ev));
    ev.events = channel->events();
    ev.data.ptr = channel;
    int fd = channel->fd();
    if(::epoll_ctl(epollfd_,operation,fd,&ev) < 0){
        if(operation == EPOLL_CTL_DEL){
            LOG_ERROR << "epoll_ctl op=" << operation << " fd=" << fd << " failed";
        }else{
            LOG_SYSERR << "epoll_ctl op=" << operation << " fd=" << fd << " failed";
        }
    }
}