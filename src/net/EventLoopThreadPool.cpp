#include "novanet/net/EventLoopThreadPool.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/EventLoopThread.h"
#include "novanet/base/Logger.h"

#include <cassert>

namespace novanet::net{

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string nameArg)
    : baseLoop_(baseLoop),
      name_(std::move(nameArg)) // 这里的 nameArg 会接收默认值或传入的值
{
}

EventLoopThreadPool::~EventLoopThreadPool(){

}

// 核心修复：接收 cb 并传递给底层的 EventLoopThread
void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
    baseLoop_->assertInLoopThread();
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        // 注：如果你之前的 EventLoopThread 构造函数支持传入名字，这里可以生成名字。
        // 我们这里遵循你之前的定义，将回调 cb 传给 EventLoopThread
        auto t = std::make_unique<EventLoopThread>(cb);
        threads_.push_back(std::move(t));
        
        // 启动子线程，并阻塞等待它内部的 EventLoop 创建完毕后返回指针
        loops_.push_back(threads_.back()->startLoop());
    }

    if (numThreads_ == 0) {
        LOG_INFO << "EventLoopThreadPool [" << name_ 
                 << "] started with 0 sub-threads. Everything falls back to baseLoop.";
    } else {
        LOG_INFO << "EventLoopThreadPool [" << name_ 
                 << "] successfully started " << numThreads_ << " sub-threads.";
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    assert(started_);
    
    EventLoop* loop = baseLoop_;

    if (!loops_.empty()) {
        // 经典的 Round-Robin (轮询) 算法
        loop = loops_[next_];
        ++next_;
        if (next_ >= loops_.size()) {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    baseLoop_->assertInLoopThread();
    assert(started_); 
    
    if (loops_.empty()) {
        return std::vector<EventLoop*>(1, baseLoop_);
    } else {
        return loops_;
    }
}


}//namespace novanet::net