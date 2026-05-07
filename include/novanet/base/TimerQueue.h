#pragma once

#include "novanet/base/Timestamp.h"
#include "novanet/base/TimerId.h"
#include "novanet/net/Channel.h"
#include <set>
#include <vector>
#include <functional>
namespace novanet{

namespace net{
class EventLoop;
class Timer;
}

/// @brief 定时器管理器
/// @brief 依托于 Linux timerfd 实现，通过 std::set 管理定时器集合.
/// 所有对外接口均是线程安全的，跨线程调用会自动派发到 loop线程。
class TimerQueue{
public:

    using TimerCallback = std::function<void()>;
    explicit TimerQueue(net::EventLoop* loop);

    ~TimerQueue();

    TimerQueue(const TimerQueue&)=delete;

    TimerQueue& operator=(const TimerQueue&) = delete;

    /// @brief 添加定时器(线程安全)
    /// @param cb 到期回调函数
    /// @param when 到期时间
    /// @param interval 重复间隔(0表示不重复)
    /// @return TimerId, 用于取消定时器
    TimerId addTimer(TimerCallback cb, Timestamp when,double interval);

    /// @brief 取消定时器(线程安全)
    void cancel(TimerId timerId);

private:
    using Entry = std::pair<Timestamp,Timer*>;
    using ActiveTimer = std::pair<Timer* ,int64_t>;

    //内部投递函数，绝对只能在 Loop 线程执行
    void addTimerInLoop(Timer* timer);
    void cancelInLoop(TimerId timerId);

    //当 timerfd 触发 EPOLLIN 时执行.
    void handleRead();

    //获取所有已经到期的定时器
    [[nodiscard]] std::vector<Entry> getExpired(Timestamp now);


    //处理这些到期的定时器(重启周期性的，销毁一次性的)
    void reset(const std::vector<Entry>& expired,Timestamp now);


    //插入到底层数据结构，如果改变了最早到期时间，返回 true
    bool insert(Timer* timer);

private:
    net::EventLoop* loop_{nullptr};
    const int timerfd_{-1};
    net::Channel timerfdChannel_;

    //核心数据结构：按到期时间，用于快速查找要触发的定时器
    std::set<Entry> timers_{};


    //用于 cancel：按内存地址和序列号保存，防 ABA问题
    std::set<ActiveTimer> activeTimers_{};

    //防止"定时器在自己的回调里取消自己"导致的灾难
    bool callingExpiredTimers_ {false};
    std::set<ActiveTimer> cancelingTimers_{};
    
};

}