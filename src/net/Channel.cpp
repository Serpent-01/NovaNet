#include "novanet/net/Channel.h"
#include "novanet/net/EventLoop.h"
#include <cassert>
#include <sys/epoll.h>
#include "novanet/base/Logger.h"

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

void Channel::tie(const std::shared_ptr<void>& obj){
    tie_ = obj;
    tied_ = true;
    LOG_INFO << "Channel (fd: " << fd_ << ") lifecycle tied.";
}


void Channel::update(){
    loop_->updateChannel(this);
}

void Channel::remove(){
    // 移除前必须确保自己不再关注任何事件
    assert(isNoneEvent());
    loop_->removeChannel(this);
}


void Channel::handleEvent(){
    eventHandling_ = true;
    //Phase3 核心：多线程下的生命周期守卫
    if(tied_){
        // 尝试将弱引用提升为强引用
        std::shared_ptr<void> guard = tie_.lock();
        if(guard){
            // 提升成功：说明宿主对象依然存活。
            // 此时 guard 增加了引用计数，回调执行期间宿主绝不会被析构。
            handleEventWithGuard();
        }else{
            // 提升失败：说明宿主对象已经在其他线程被释放。
            // 此时绝对不能执行回调，直接丢弃事件！
            LOG_WARN << "Channel (fd: " << fd_ << ") owner object has been destroyed, drop event handling.";
        }
    }else{
        // 未绑定 tie 的 Channel（例如 Acceptor 监听新连接的 fd，它的生命周期是由 TcpServer 静态持有的）
        // 可以直接执行
        handleEventWithGuard();
    }
    eventHandling_ = false;
}

void Channel::handleEventWithGuard(){
    // 1. 处理对端挂断 (EPOLLHUP) 且没有数据可读的情况
    if((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)){
        LOG_WARN << "fd = " << fd_ << " Channel::handleEventWithGuard() EPOLLHUP (Hang up)";
        if(closeCallback_){
            closeCallback_();
        }
    }
    // 2. 处理错误事件 (EPOLLERR)

    if(revents_ & EPOLLERR){
        LOG_ERROR << "fd = " << fd_ << " Channel::handleEventWithGuard() EPOLLERR";
        if(errorCallback_){
            errorCallback_();
        }
    }

    // 3. 处理可读事件 (EPOLLIN: 普通数据, EPOLLPRI: 带外数据, EPOLLRDHUP: 对端关闭连接的一半)
    if(revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)){
        if(readCallback_){
            readCallback_();
        }
    }
    // 4. 处理可写事件 (EPOLLOUT)
    if(revents_ & EPOLLOUT){
        if(writeCallback_){
            writeCallback_();
        }
    }

}

// void Channel::handleEvent(){
//     eventHandling_ = true;

    

//     if((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)){

//         LOG_WARN << "fd = " << fd_ << " Channel::handleEvent() EPOLLHUP";

//         if(closeCallback_){
//             closeCallback_();
//         }

//         eventHandling_ = false;
//         return;
//     }

//     if(revents_ & EPOLLERR){
//         LOG_ERROR << "fd = " << fd_ << " Channel::handleEvent() EPOLLERR";
//         if(errorCallback_){
//             errorCallback_();
//         }
//     }

//     if(revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)){
//         if(readCallback_){
//             readCallback_();
//         }
//     }

//     if(revents_ & EPOLLOUT){
//         if(writeCallback_){
//             writeCallback_();
//         }
//     }
//     eventHandling_ = false;
// }