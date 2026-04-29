#pragma once

#include <functional>
#include <utility>

namespace novanet::net{
class EventLoop;

/**
 * @brief 核心组件：事件分发器 (Event Dispatcher)
 * * 架构定位：
 * - Channel 是具体文件描述符(fd)的“保姆”或“通讯员”。
 * - 它不拥有 fd 的生命周期（不负责 close(fd)），只负责封装 fd 的【事件语义】。
 * - 它是底层 epoll 机制与上层业务回调之间的桥梁。
 */
class Channel{
public:
    using EventCallback = std::function<void()>;
    Channel(EventLoop* loop,int fd);

    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    // 核心流转枢纽：当 epoll_wait 返回活跃事件时，由 EventLoop 调用此函数，
    // 此函数再根据底层实际发生的事件 (revents_)，分发到具体的业务回调中。
    void handleEvent();
    
    // 绑定业务层注入的回调逻辑
    void setReadCallback(EventCallback cb) {readCallback_ = std::move(cb);}
    void setWriteCallback(EventCallback cb){writeCallback_ = std::move(cb);}
    void setCloseCallback(EventCallback cb) {closeCallback_ = std::move(cb);}
    void setErrorCallback(EventCallback cb) {errorCallback_ = std::move(cb);}

    // 状态查询接口
    int fd() const{
        return fd_;
    }

    int events()const{
        return events_;
    }
    int revents() const{
        return revents_;
    }

    void set_revents(int revt){
        revents_ = revt;
    }
    bool isNoneEvent() const{
        return events_ == kNoneEvent;
    }
    
    void enableReading(){
        events_ |= kReadEvent;
        update();
    }
    void disableReading(){
        events_ &= ~kReadEvent;
        update();
    }

    void enableWriting(){
        events_ |= kWriteEvent;
        update();
    }
    void disableWriting(){
        events_ &= ~kWriteEvent;
        update();
    }

    void disableAll(){
        events_ = kNoneEvent;
        update();
    }
    
    bool isWriting() const{
        return (events_ & kWriteEvent) != 0;
    }

    bool isReading() const{
        return (events_ & kReadEvent) != 0;
    }

    // 供 Poller 使用的状态锚点 (非常重要)
    // index_ 记录了当前 Channel 在 Poller 中的挂载状态：
    // -1: 新建，还未添加到 epoll
    //  1: 已添加 (EPOLL_CTL_ADD)
    //  2: 已删除 (EPOLL_CTL_DEL)
    int index () const{
        return index_;
    }

    void set_index(int idx){
        index_ = idx;
    }

    EventLoop* ownerLoop() const{
        return loop_;
    }

    // 从 EventLoop 和 Poller 中彻底移除自己
    void remove();

private:    
    // 将自己对事件的最新诉求，委托给 EventLoop 传递给底层的 Poller 进行 epoll_ctl
    void update();

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

private:
    EventLoop* loop_; // 所属的大管家 (单线程 Reactor 核心)
    const int fd_;  // 被监听的文件描述符

    int events_;
    int revents_;
    int index_;     // 在 Poller 中的状态机索引

    bool eventHandling_; // 防御性标志：当前是否正在执行回调函数

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};


}//namespace novanet::net