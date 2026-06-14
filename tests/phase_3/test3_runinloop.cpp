#include "novanet/net/EventLoop.h"
#include "novanet/net/EventLoopThread.h"
#include "novanet/base/Logger.h"

#include <thread>
#include <unistd.h>

using namespace novanet::net;
// using namespace novanet::base; // 如果 Logger 在 base 命名空间下

EventLoop* g_mainLoop = nullptr;

// 这个函数将在 Main Loop 的线程内执行
void testSameThreadSemantics() {
    LOG_INFO << "========== 测试 1 & 2：同线程下的行为 ==========";
    LOG_INFO << "当前执行线程 ID: " << std::this_thread::get_id();

    // 测试 1: 同线程调用 runInLoop
    LOG_INFO << "[Test 1] 准备调用 runInLoop...";
    g_mainLoop->runInLoop([]() {
        LOG_INFO << "    >>> [执行中] runInLoop 的回调函数被执行了！";
    });
    LOG_INFO << "[Test 1] runInLoop 调用完毕。";

    LOG_INFO << "--------------------------------------------------";

    // 测试 2: 同线程调用 queueInLoop
    LOG_INFO << "[Test 2] 准备调用 queueInLoop...";
    g_mainLoop->queueInLoop([]() {
        LOG_INFO << "    >>> [执行中] queueInLoop 的回调函数被执行了！";
    });
    LOG_INFO << "[Test 2] queueInLoop 调用完毕。";
}

int main() {
    // 开启 INFO 日志
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    LOG_INFO << "Main 线程 ID: " << std::this_thread::get_id();

    EventLoop mainLoop;
    g_mainLoop = &mainLoop;

    // 1. 我们先把 testSameThreadSemantics 塞进主循环，等主循环跑起来后执行
    mainLoop.queueInLoop(testSameThreadSemantics);

    // 2. 开启一个子线程 Loop
    EventLoopThread loopThread;
    EventLoop* subLoop = loopThread.startLoop(); // 阻塞直到子线程启动完毕
    
    // 稍微等一下，让前面的日志打印完，避免混淆
    ::usleep(100000); 

    LOG_INFO << "========== 测试 3：跨线程下的行为 ==========";
    LOG_INFO << "[Test 3] Main 线程准备向 Sub Loop 投递 runInLoop...";
    
    // 测试 3: 跨线程调用 runInLoop
    subLoop->runInLoop([]() {
        LOG_INFO << "    >>> [执行中] 跨线程的 runInLoop 回调被执行！当前线程 ID: " << std::this_thread::get_id();
    });
    
    LOG_INFO << "[Test 3] Main 线程 runInLoop 代码行执行完毕。";

    // 启动主循环
    mainLoop.loop();

    return 0;
}