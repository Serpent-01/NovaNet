#include "novanet/net/EventLoop.h"
#include "novanet/net/EventLoopThread.h"
#include "novanet/base/Timestamp.h"
#include "novanet/base/Logger.h"

#include <thread>
#include <chrono>
#include <unistd.h>

/*
    必测 4：eventfd 能唤醒阻塞中的 loop
*/
using namespace novanet::net;
// using namespace novanet::base; // 视你的命名空间而定

void printTimestampInfo(const char* msg, novanet::base::Timestamp time) {
    LOG_INFO << msg << " 发生时间: " << time.toFormattedString();
}

int main() {
    // 开启 INFO 日志，便于观察 Poller 和 EventLoop 的底层打印
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    LOG_INFO << "========== 开启 EventFd 唤醒能力极限界测试 ==========";
    LOG_INFO << "主线程 ID: " << std::this_thread::get_id();

    // 1. 启动一个干干净净的子线程 Loop（没有任何网络 Connection 挂在上面）
    EventLoopThread loopThread;
    EventLoop* subLoop = loopThread.startLoop();

    // 2. 主线程故意休眠 3 秒钟
    // 此时，子线程因为没有任何事件，必然会陷入 epoll_wait 的死寂阻塞中
    LOG_INFO << "[Main] 主线程开始休眠 3 秒，让子线程彻底陷入 epoll_wait 阻塞...";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOG_INFO << "[Main] 主线程苏醒！";

    // 3. 记录主线程投递任务的精确时间
    novanet::base::Timestamp enqueueTime = novanet::base::Timestamp::now();
    printTimestampInfo("[Main] 准备触发 queueInLoop (敲响 eventfd)", enqueueTime);

    // 4. 从主线程向【正在阻塞的子线程】投递任务
    subLoop->queueInLoop([enqueueTime]() {
        // 5. 记录子线程实际开始执行任务的时间
        novanet::base::Timestamp executeTime = novanet::base::Timestamp::now();
        
        LOG_INFO << "    >>> [Sub] 子线程被唤醒并执行了任务！当前线程 ID: " << std::this_thread::get_id();
        printTimestampInfo("    >>> [Sub] 任务实际执行时间", executeTime);
        
        // 计算唤醒延迟 (微秒)
        double delayMicroseconds = timeDifference(executeTime, enqueueTime) * 1000000;
        LOG_INFO << "    >>> [Sub] eventfd 跨线程唤醒延迟: " << delayMicroseconds << " 微秒";
    });

    // 主线程等一等，让子线程有时间打印日志
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO << "测试结束。准备退出。";
    subLoop->quit(); // 优雅关闭子线程 Loop

    return 0;
}