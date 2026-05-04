#include "novanet/net/EventLoop.h"
#include "novanet/net/Channel.h"
#include <iostream>
#include <cassert>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>

using namespace std;
using namespace novanet::net;

// 辅助函数：创建一个 eventfd
int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(evtfd >= 0);
    return evtfd;
}

// 辅助函数：创建一个 timerfd (100毫秒后触发)
int createTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec howlong;
    bzero(&howlong, sizeof howlong);
    howlong.it_value.tv_sec = 0;
    howlong.it_value.tv_nsec = 100 * 1000 * 1000; // 100ms
    ::timerfd_settime(timerfd, 0, &howlong, NULL);
    return timerfd;
}

int main() {
    cout << "========== 开启 Reactor 核心测试 ==========" << endl;

    EventLoop loop;
    int evfd = createEventfd();
    Channel channel(&loop, evfd);

    int readCallbackCount = 0;
    int writeCallbackCount = 0;

    // --- 设置 Channel 的回调 ---
    channel.setReadCallback([&]() {
        cout << "[Channel] 读事件触发!" << endl;
        uint64_t one;
        ssize_t n = ::read(evfd, &one, sizeof one);
        assert(n == sizeof one);
        readCallbackCount++;

        // 必测 9: 测试事件开关
        channel.disableReading(); // 关掉读
        channel.enableWriting();  // 开启写 (必测 10: 这里会触发 Poller 的 mod)
    });

    channel.setWriteCallback([&]() {
        cout << "[Channel] 写事件触发!" << endl;
        writeCallbackCount++;
        
        // 必测 9: 写完必须立刻关掉，否则死循环
        channel.disableWriting(); 
        // 测试移除 (必测 10: 触发 Poller 的 del)
        channel.remove(); 
    });

    // 必测 10: 首次 add
    channel.enableReading(); 

    // 手动往 eventfd 写数据，以触发第一次读事件
    uint64_t one = 1;
    ::write(evfd, &one, sizeof one);

    // --- 设置一个定时器用于退出 (必测 11) ---
    int tfd = createTimerfd();
    Channel timerChannel(&loop, tfd);
    timerChannel.setReadCallback([&]() {
        cout << "[Timer] 100ms 时间到，准备退出 EventLoop" << endl;
        uint64_t val;
        ::read(tfd, &val, sizeof val);
        loop.quit(); // 必测 11: quit() 正常退出
    });
    timerChannel.enableReading();

    // 必测 11: 启动死循环
    cout << "[EventLoop] 开始 loop()" << endl;
    loop.loop(); 
    cout << "[EventLoop] 成功退出 loop()" << endl;

    // --- 验收标准断言 ---
    assert(readCallbackCount == 1);  // 应该且只触发 1 次读
    assert(writeCallbackCount == 1); // 开启写后，应该且只触发 1 次写

    ::close(evfd);
    ::close(tfd);
    cout << "[PASS] 必测 9, 10, 11 (Channel / Poller / EventLoop) 完美通关！" << endl;
    return 0;
}