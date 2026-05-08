#include "novanet/net/TimerQueue.h"
#include "novanet/base/Timestamp.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/Timer.h"
#include "novanet/base/Logger.h"


#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <cassert>

using namespace novanet::net;

// 匿名命名空间，屏蔽内部 C 风格工具函数
namespace{ 

int createTimerfd(){
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timerfd < 0){
        LOG_SYSFATAL << "Failed to create timerfd";
    }
    return timerfd;
}   


//计算到期时间与当前时间的差值，转换为 timerfd需要的 timespec 格式
struct timespec howMuchTimerFromNow(Timestamp when){
    int64_t microseconds = when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();

    if(microseconds < 100){
        microseconds = 100; //最低保护：哪怕已经过期，也至少延迟 100 微秒触发，防死循环
    }
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microseconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>((microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

//“消耗”（Consume）或者说“重置”定时器的可读事件，防止事件循环
//当 timerfd 设置的时间到达（定时器超时）时，操作系统会将这个文件描述符标记为可读
//Linux 系统规定，当从 timerfd 中读取数据时，必须提供一个 8 字节的缓冲区
void readTimerfd(int timerfd,Timestamp now){
    uint64_t howmany;
    ssize_t n = ::read(timerfd,&howmany,sizeof(howmany));
    
    if(n != sizeof(howmany)){
        LOG_SYSERR << "TimerQueue::readTimerfd() reads " << n << " bytes instead of 8";
    }
}
//告诉 Linux 内核，我的 timerfd 下一次应该在什么时候“响”（变得可读），从而唤醒底层的 epoll
void resetTimerfd(int timerfd, Timestamp expiration){
    struct itimerspec newValue;
    struct itimerspec oldValue;
    std::memset(&newValue,0,sizeof(newValue));
    std::memset(&oldValue,0,sizeof(oldValue));

    newValue.it_value = howMuchTimerFromNow(expiration);
    int ret = ::timerfd_settime(timerfd,0,&newValue,&oldValue);
    if(ret != 0){
        LOG_SYSERR << "timerfd_settime failed";
    }
}

}// namespace


TimerQueue::TimerQueue(net::EventLoop* loop)
     : loop_(loop),
        timerfd_(createTimerfd()),
        timerfdChannel_(loop,timerfd_) {
    timerfdChannel_.setReadCallback(std::bind(&TimerQueue::handleRead,this));
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue(){
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);

    for(const auto& timer : timers_){
        delete timer.second;
    }
}

TimerId TimerQueue::addTimer(Timer::TimerCallback cb,Timestamp when, double interval){
    Timer* timer = new Timer(std::move(cb),when,interval);

    loop_->runInLoop([this,timer](){
        this->addTimerInLoop(timer);
    });

    return TimerId(timer,timer->sequence());
}

void TimerQueue::addTimerInLoop(Timer* timer){
    loop_->assertInLoopThread();

    bool earliestChanged = insert(timer);

    if(earliestChanged){
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerQueue::cancel(TimerId timerId){
    loop_->runInLoop([this,timerId](){
        this->cancelInLoop(timerId);
    });
}


void TimerQueue::cancelInLoop(TimerId timerId){
    loop_->assertInLoopThread();
    assert(timers_.size() == activeChannels_.size());

    ActiveTimer timer(timerId.timer_,timerId.sequence_);

    auto it = activeTimers_.find(timer);

    if(it != activeTimers_.end()){
        size_t n = timers_.erase(Entry(it->first->expiration(),it->first));
        assert(n == 1);
        delete it->first;
        activeTimers_.erase(it);
    }else if(callingExpiredTimers_){
        cancelingTimers_.insert(timer);
    }
}


void TimerQueue::handleRead(){
    loop_->assertInLoopThread();
    Timestamp now(Timestamp::now());
    readTimerfd(timerfd_,now);

    std::vector<Entry> expired = getExpired(now);

    callingExpiredTimers_ = true;

    cancelingTimers_.clear();

    for(const auto& entry : expired){
        entry.second->run();
    }
    callingExpiredTimers_ = false;

    reset(expired,now);
}


std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now){
    std::vector<Entry> expired;

    Entry sentry(now,reinterpret_cast<Timer*>(UINTPTR_MAX));
    auto end = timers_.lower_bound(sentry);

    std::copy(timers_.begin(),end,std::back_inserter(expired));
    
    timers_.erase(timers_.begin(),end);

    for(const auto& entry : expired){
        ActiveTimer timer(entry.second,entry.second->sequence());
        size_t n = activeTimers_.erase(timer);
        assert(n == 1);
    }
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired,Timestamp now){
    Timestamp nextExpire;

    for(const auto& entry : expired){
        ActiveTimer timer(entry.second,entry.second->sequence());

        if(entry.second->repeat() && cancelingTimers_.find(timer) == cancelingTimers_.end()){
            entry.second->restart(now);
            insert(entry.second);
        }else{
            delete entry.second;
        }
    }
    if(!timers_.empty()){
        nextExpire = timers_.begin()->second->expiration();
    }
    if(nextExpire.valid()){
        resetTimerfd(timerfd_, nextExpire);
    }
}


bool TimerQueue::insert(Timer* timer){
    loop_->assertInLoopThread();
    assert(timers_.size() == activeTimers_.size());
    bool earliestChanged = false;
    Timestamp when = timer->expiration();
    auto it = timers_.begin();
    if(it == timers_.end() || when < it->first){
        earliestChanged = true;
    }
    timers_.insert(Entry(when,timer));
    activeTimers_.insert(ActiveTimer(timer,timer->sequence()));

    assert(timers_.size() == activeTimers_.size());
    return earliestChanged;
}



