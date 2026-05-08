#include "novanet/net/EventLoop.h"
#include "novanet/base/Timestamp.h"
#include "novanet/net/Poller.h"
#include "novanet/net/Channel.h"
#include "novanet/base/Logger.h"
#include "novanet/net/TimerQueue.h"

#include <memory>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <mutex>
#include <thread>

using namespace novanet;
using namespace novanet::net;
namespace{


thread_local EventLoop* t_loopInThisThread = nullptr;
const int kPollTimeMs = 10000;

//创建用于线程间唤醒的 eventfd

int createEventfd(){
    int evtfd = ::eventfd(0,EFD_NONBLOCK | EFD_CLOEXEC);
    if(evtfd < 0){
        LOG_SYSFATAL << "Failed in eventfd";
    }
    return evtfd;
}

}

EventLoop::EventLoop()
    :poller_(std::make_unique<Poller> (this)),
    timerQueue_(std::make_unique<TimerQueue>(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(std::make_unique<Channel>(this,wakeupFd_)) {
    
    LOG_INFO << "EventLoop created in thread" << threadId_;

    if(t_loopInThisThread){
        LOG_SYSFATAL << "Another EventLoop already exists in this thread " << threadId_;
    }else{
        t_loopInThisThread = this;
    }

    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead,this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop(){
    LOG_INFO << "EventLoop destroyed in thread " << threadId_;
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop(){
    assert(!looping_);
    assertInLoopThread();//是否所属线程

    looping_ = true;
    quit_ = false;

    LOG_INFO << "EventLoop starting looping";

    while(!quit_){
        activeChannels_.clear();
        poller_->poll(kPollTimeMs,&activeChannels_);
        eventHandling_ = true;
        for(Channel* channel : activeChannels_){
            currentActiveChannel_ = channel;
            currentActiveChannel_->handleEvent();
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;
        doPendingFunctors();
    }
    LOG_INFO << "EventLoop stop looping";
    looping_ = false;
}

void EventLoop::quit(){
    quit_ = true;
    if (!isInLoopThread()) {
        wakeup(); 
    }
}


void EventLoop::runInLoop(Functor cb){
    if(isInLoopThread()){
        cb();
    }else{
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb){
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    //投递任务后，去唤醒目标线程
    if(!isInLoopThread() || callingPendingFunctors_){
        wakeup();
    }
}

void EventLoop::doPendingFunctors(){
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for(const Functor& functor : functors){
        functor();
    }
    callingPendingFunctors_ = false;
}


// 定时器
TimerId EventLoop::runAt(Timestamp time,Functor cb){
    return timerQueue_->addTimer(cb, time, 0.0);
}

TimerId EventLoop::runAfter(double delay,Functor cb){
    // Timestamp addTime
    Timestamp time = base::addTime(base::Timestamp::now(),delay);
    return runAt(time,std::move(cb));
}

TimerId EventLoop::runEvery(double interval, Functor cb){
    base::Timestamp time = base::addTime(base::Timestamp::now(), interval);
    return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId){
    return timerQueue_->cancel(timerId);
}


void EventLoop::wakeup(){
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_,&one,sizeof(one));
    if(n != sizeof(one)){
        LOG_SYSERR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
    }
}

void EventLoop::handleRead(){
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd_,&one,sizeof(one));
    if(n != sizeof(one)){
        LOG_SYSERR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
    }
}

void EventLoop::updateChannel(Channel* channel){
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel){
    assert(channel->ownerLoop() == this);
    assertInLoopThread();

    if(eventHandling_){
        assert(currentActiveChannel_ == channel ||
                std::find(activeChannels_.begin(), activeChannels_.end(),channel) == activeChannels_.end());
    }
    
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel){
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

bool EventLoop::isInLoopThread() const{
    return threadId_ == std::this_thread::get_id();
}


void EventLoop::assertInLoopThread() const{
    if(!isInLoopThread()){
        LOG_SYSFATAL << "EventLoop::assertInLoopThread - EventLoop was created in threadId_ = " 
                     << threadId_ << ", but current thread id = " << std::this_thread::get_id();
    }
}