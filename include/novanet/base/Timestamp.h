#pragma once

#include <cstdint>
#include <string>

namespace novanet::base{

/// @brief 时间戳，精确到微秒 (10^-6 秒)。
/// @note 内部封装为 64 位整数，8 字节大小。推荐按值传递 (Pass-by-value)。
class Timestamp {
public:
    static constexpr int64_t kMicroSecondsPerSecond = 1000 * 1000;

    // 默认构造一个无效的时间戳 (0)
    constexpr Timestamp() noexcept : microSecondsSinceEpoch_(0) {}

    // explicit 防止隐式转换引发的逻辑错误
    constexpr explicit Timestamp(int64_t microSecondsSinceEpoch) noexcept
        : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

    // C++17: 强制调用者检查返回值，防止写出 timestamp.valid(); 这样的废代码
    [[nodiscard]] bool valid() const noexcept { return microSecondsSinceEpoch_ > 0; }
    
    [[nodiscard]] int64_t microSecondsSinceEpoch() const noexcept { return microSecondsSinceEpoch_; }
    
    [[nodiscard]] time_t secondsSinceEpoch() const noexcept {
        return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
    }

    // 转换为字符串，通常用于日志记录
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toFormattedString(bool showMicroseconds = true) const;

    // 获取当前时间
    [[nodiscard]] static Timestamp now();
    
    // 获取一个无效时间
    [[nodiscard]] static Timestamp invalid() { return Timestamp(); }

private:
    int64_t microSecondsSinceEpoch_;
};

// ---------------- 友元与非成员函数 ----------------
// C++ 中重载比较运算符的最佳实践是定义为 inline 的非成员函数

inline bool operator<(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

inline bool operator<=(Timestamp lhs, Timestamp rhs) noexcept { return !(rhs < lhs); }
inline bool operator>(Timestamp lhs, Timestamp rhs) noexcept { return rhs < lhs; }
inline bool operator>=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs < rhs); }
inline bool operator!=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs == rhs); }

/// @brief 计算两个时间戳的差值，返回以秒为单位的 double
inline double timeDifference(Timestamp high, Timestamp low) noexcept {
    int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;
}

/// @brief 给时间戳加上指定的秒数，返回新的时间戳
inline Timestamp addTime(Timestamp timestamp, double seconds) noexcept {
    int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
    return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

}