#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace novanet::net{

class EventLoop;

class EventLoopThread{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    
    explicit EventLoopThread(ThreadInitCallback cb = nullptr);
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 启动线程，阻塞等待直到 EventLoop 在子线程内部创建完毕
    EventLoop* startLoop();

    // 请求 EventLoop 退出，并等待底层线程结束。
    void stopAndJoin();

private: 
    
    void threadFunc();

    EventLoop* loop_{nullptr};
    bool exiting_ {false};
    
    std::thread thread_;
    std::mutex joinMutex_;
    std::mutex mutex_;
    std::condition_variable cond_;

    ThreadInitCallback callback_;

};

}
