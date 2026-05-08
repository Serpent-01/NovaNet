#include "novanet/net/EventLoopThreadPool.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/EventLoopThread.h"
#include "novanet/base/Logger.h"

#include <cassert>

namespace novanet::net{

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop,std::string nameArg)
    : baseLoop_(baseLoop),name_(std::move(nameArg)) {

}

EventLoopThreadPool::~EventLoopThreadPool(){

}

void EventLoopThreadPool::start(){
    baseLoop_->assertInLoopThread();
    started_ = true;

    for(int i = 0;i<numThreads_;i++){
        auto t = std::make_unique<EventLoopThread>();
        threads_.push_back(std::move(t));
        loops_.push_back(threads_.back()->startLoop());
    }
    if(numThreads_ == 0){
        LOG_INFO << "EventLoopThreadPool [" << name_ << "] started with 0 sub-threads. Everything falls back to baseLoop.";
    }else {
        LOG_INFO << "EventLoopThreadPool [" << name_ << "] successfully started " << numThreads_ << " sub-threads.";
    }
}

EventLoop* EventLoopThreadPool::getNextLoop(){
    
    baseLoop_->assertInLoopThread();
    assert(started_);
    EventLoop* loop = baseLoop_;

    if(!loops_.empty()){
        // 经典的 Round-Robin (轮询) 算法
        loop = loops_[next_];
        ++next_;
        if(next_ >= loops_.size()){
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const{
    baseLoop_->assertInLoopThread();
    assert(strated_);
    if(loops_.empty()){
        return std::vector<EventLoop*> (1,baseLoop_);
    }else{
        return loops_;
    }
}


}//namespace novanet::net