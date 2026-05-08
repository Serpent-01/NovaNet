#include "novanet/net/EventLoopThread.h"
#include "novanet/net/EventLoop.h"
// 假设你的日志头文件路径如下，按实际情况调整
#include "novanet/base/Logger.h" 
#include <cassert>

using namespace novanet;
using namespace novanet::net;

EventLoopThread::EventLoopThread(ThreadInitCallback cb) :callback_(std::move(cb)) {
    LOG_INFO << "EventLoopThread object created.";
}

EventLoopThread::~EventLoopThread(){
    exiting_ = true;
    if(loop_ != nullptr){
        LOG_INFO << "EventLoopThread destructor: sending quit signal to EventLoop...";
        loop_->quit();
        if(thread_.joinable()){
            thread_.join();
            LOG_INFO << "EventLoopThread destructor: underlying thread joined successfully.";
        }
    }
}

EventLoop* EventLoopThread::startLoop(){
    LOG_INFO << "EventLoopThread::startLoop() called, spawning new thread...";
    
    thread_ = std::thread(&EventLoopThread::threadFunc,this);

    EventLoop* loop = nullptr;

    // 阻塞等待子线程真正启动并完成 EventLoop 的栈上构造
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while(loop_ == nullptr){
            cond_.wait(lock);
        }
        loop = loop_;
    }
    LOG_INFO << "EventLoopThread::startLoop() finished waiting. EventLoop address: " << loop;
    assert(loop != nullptr);
    return loop;
}

void EventLoopThread::threadFunc(){
    // 这里非常关键：在子线程局部作用域内构造 EventLoop
    // 它内部会调用 std::this_thread::get_id() 绑定当前线程
    EventLoop loop;
    LOG_INFO << "EventLoop created inside new thread.";

    if(callback_){
        LOG_INFO << "Executing ThreadInitCallback...";
        callback_(&loop);
    }
    // 初始化完毕，通知主线程
    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
        LOG_INFO << "EventLoop pointer exposed and cond_ notified.";
    }
    // ====== 开启事件循环 ======
    LOG_INFO << "EventLoop starting looping in sub-thread...";
    loop.loop();

    // 运行到这里说明收到了 quit() 并且所有事件处理完毕
    LOG_INFO << "EventLoop stopped looping, sub-thread exiting...";

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = nullptr; // 防止主线程持有野指针
    }
}