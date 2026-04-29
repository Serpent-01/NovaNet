#include "novanet/net/EventLoop.h"
#include "novanet/net/Poller.h"
#include "novanet/net/Channel.h"
#include "novanet/base/Logger.h"

#include <cassert>
#include <mutex>
#include <thread>

using namespace novanet::net;

thread_local EventLoop* t_loopInThisThread = nullptr;

const int kPollTimeMs = 10000;

EventLoop::EventLoop()
    :looping_(),
    quit_(false),
    callingPendingFunctors_(false),
    threadId_(std::this_thread::get_id()),
    poller_(std::make_unique<Poller> (this)),
    currentActiveChannel_(nullptr) {
    
    LOG_INFO << "EventLoop created in thread" << threadId_;

    if(t_loopInThisThread){
        LOG_SYSFATAL << "Another EventLoop already exists in this thread " << threadId_;
    }else{
        t_loopInThisThread = this;
    }
}

EventLoop::~EventLoop(){
    LOG_INFO << "EventLoop destroyed in thread " << threadId_;
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

        for(Channel* channel : activeChannels_){
            currentActiveChannel_ = channel;
            currentActiveChannel_->handleEvent();
        }
        currentActiveChannel_ = nullptr;

        doPendingFunctors();
    }
    LOG_INFO << "EventLoop stop looping";
    looping_ = false;
}

void EventLoop::quit(){
    quit_ = true;
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

void EventLoop::updateChannel(Channel* channel){
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel){
    assert(channel->ownerLoop() == this);
    assertInLoopThread();

    if(currentActiveChannel_ == channel){

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