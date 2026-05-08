#pragma once

#include "novanet/base/Timestamp.h"
#include <atomic>
#include <functional>
using namespace novanet::base;
namespace novanet::net{

/// @brief 定时器实体类
/// @note 封装了回调函数、到期时间、重复间隔和全局唯一序列号。
///       该类不可拷贝、不可移动。
class Timer{
public:
    using TimerCallback = std::function<void()>;
    Timer(TimerCallback cb,Timestamp when,double interval)
        :callback_(std::move(cb)),expiration_(when),
        interval_(interval),repeat_(interval > 0.0),
        sequence_(s_numCreated_.fetch_add(1,std::memory_order_relaxed)){}
    ~Timer() = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    //执行回调
    void run() const{
        if(callback_){
            callback_();
        }
    }
    //重置定时器
    void restart(Timestamp now);

    [[nodiscard]] Timestamp expiration() const noexcept {return expiration_;}
    [[nodiscard]] bool repeat() const noexcept{return repeat_;}
    [[nodiscard]] int64_t sequence() const noexcept{return sequence_;}

    
private:
    const TimerCallback callback_{nullptr};
    Timestamp expiration_{Timestamp::invalid()};
    const double interval_ {0.0};
    const bool repeat_ {false};
    const int64_t sequence_ {0};

    static std::atomic<int64_t> s_numCreated_;
};

}