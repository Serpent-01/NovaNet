#include "novanet/base/TimerQueue.h"
#include "novanet/base/Timestamp.h"
#include "novanet/net/EventLoop.h"
#include "novanet/base/Timer.h"
#include "novanet/base/Logger.h"


#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <cassert>

namespace novanet{

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

TimerId TimerQueue::addTimer(Timer::TimerCallback cb, Timestamp when, double interval){
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


}// namespace novanet
