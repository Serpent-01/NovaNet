#include "novanet/net/EventLoop.h"
#include "novanet/base/Timestamp.h"
#include "novanet/base/Logger.h"
#include <thread>
#include <chrono>

using namespace novanet;
using namespace novanet::net;
using namespace novanet::base;

EventLoop* g_loop = nullptr;

// ---------------------------------------------------------
// 测试 1：跨线程唤醒测试
// ---------------------------------------------------------
void threadFunc() {
    LOG_INFO << "Worker thread started. Thread ID: " << std::this_thread::get_id();
    
    // 模拟一段耗时的非 I/O 任务
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    LOG_INFO << "Worker thread finished work, waking up main loop...";
    
    // 🌟 核心测试点：跨线程投递任务，触发 eventfd 唤醒
    g_loop->queueInLoop([]() {
        LOG_INFO << ">>> This task was queued by worker thread, but is running in loop thread: " 
                 << std::this_thread::get_id();
    });
}

// ---------------------------------------------------------
// 测试 2：定时器功能综合测试
// ---------------------------------------------------------
int count = 0;
TimerId periodicTimerId; // 用于保存周期定时器的 ID，以便后续取消

void printTid() {
    LOG_INFO << "Timer triggered! Thread ID: " << std::this_thread::get_id();
}

void printEvery() {
    LOG_INFO << "Periodic timer triggered! Count: " << ++count;
    if (count == 5) {
        LOG_INFO << "Canceling periodic timer...";
        g_loop->cancel(periodicTimerId); // 🌟 核心测试点：取消定时器
    }
}

int main() {
    // 设置日志级别为 INFO，方便观察输出
    Logger::setLogLevel(LogLevel::Info);
    
    LOG_INFO << "Main thread started. Thread ID: " << std::this_thread::get_id();

    // 1. 创建 EventLoop
    EventLoop loop;
    g_loop = &loop;

    // 2. 启动一个工作线程，测试跨线程投递 (queueInLoop)
    std::thread workerThread(threadFunc);

    // 3. 注册各类定时器，测试 TimerQueue 的准确性
    LOG_INFO << "Registering timers...";
    
    // 1秒后执行一次
    loop.runAfter(1.0, []() {
        LOG_INFO << ">>> Timer [runAfter 1.0s] executed.";
    });

    // 3秒后执行一次
    loop.runAfter(3.0, []() {
        LOG_INFO << ">>> Timer [runAfter 3.0s] executed.";
    });

    // 每 1.5 秒执行一次
    periodicTimerId = loop.runEvery(1.5, printEvery);

    // 10秒后退出整个事件循环
    loop.runAfter(10.0, [&loop]() {
        LOG_INFO << ">>> 10 seconds passed. Quitting EventLoop...";
        loop.quit();
    });

    // 4. 正式启动事件循环（此时主线程将阻塞在 epoll_wait 中）
    LOG_INFO << "EventLoop::loop() called. Main thread is now blocking...";
    loop.loop();

    LOG_INFO << "EventLoop exited gracefully.";

    // 收拾残局
    workerThread.join();
    return 0;
}