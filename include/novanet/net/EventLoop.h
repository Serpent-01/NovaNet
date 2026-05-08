#pragma once
#include "novanet/base/Timestamp.h" // 引入 Timestamp
#include "novanet/net/TimerId.h"    // 引入 TimerId
#include <vector>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>


using namespace novanet::base;
namespace novanet::net{
class TimerQueue;
class Channel;
class Poller;


/// @brief 事件循环核心类 (Reactor)
/// @note 遵循 One loop per thread 模型。支持跨线程任务投递与 eventfd 唤醒。
class EventLoop{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();


    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();

    void quit();

    // 跨线程调度核心
    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    void wakeup();

    // 定时器 
    TimerId runAt(Timestamp timer,Functor cb);
    TimerId runAfter(double delay,Functor cb);
    TimerId runEvery(double interval,Functor cb);
    void cancel(TimerId timerId);

    // Channel 管理
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    [[nodiscard]] bool hasChannel(Channel* channel);


    // 线程校验
    [[nodiscard]] bool isInLoopThread() const;
    void assertInLoopThread() const;

private:
    
    void handleRead(); //eventfd 唤醒回调
    void doPendingFunctors();

private:
    using ChannelList = std::vector<Channel*>;

    bool looping_ {false};

    std::atomic<bool> quit_ {false};
    bool eventHandling_{false};
    bool callingPendingFunctors_{false};

    const std::thread::id threadId_ {std::this_thread::get_id()};

    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;//管理所有定时器

    const int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;


    ChannelList activeChannels_{};
    Channel* currentActiveChannel_{nullptr};
    
    mutable std::mutex mutex_;

    std::vector<Functor> pendingFunctors_{};
};

}