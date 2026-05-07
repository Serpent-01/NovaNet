#pragma once

#include <cstdint>

namespace novanet{

class Timer;


class TimerId{

public:
    constexpr TimerId() noexcept = default;

    constexpr TimerId(Timer* timer,int64_t sequence) noexcept :timer_(timer),sequence_(sequence) {}

    ~TimerId() = default;

    [[nodiscard]] bool valid() const noexcept{
        return timer_ != nullptr;
    }
    friend class TimerQueue;
private:
    Timer* timer_ {nullptr};
    int64_t sequence_ {0};
};


}