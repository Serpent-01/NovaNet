#include "novanet/base/Timestamp.h"

#include <chrono>
#include <cstdio>
#include <cinttypes>

using namespace novanet;

Timestamp Timestamp::now(){

    auto now = std::chrono::system_clock::now();
    
    auto duration = now.time_since_epoch();

    int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    return Timestamp(microseconds);
}

std::string Timestamp::toString() const{
    char buf[32] = {0};
    
    int64_t seconds = microSecondsSinceEpoch_ / kMicroSecondsPerSecond;
    int64_t microseconds = microSecondsSinceEpoch_ % kMicroSecondsPerSecond;  
    
    std::snprintf(buf,sizeof(buf),"%" PRId64 ".%06" PRId64 "",seconds,microseconds);
    return std::string(buf);
}

std::string Timestamp::toFormattedString(bool showMicroseconds) const{
    char buf[64] = {0};
    time_t seconds = static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
    struct tm tm_time;


    // 在 Linux 环境下，必须使用带 _r (reentrant) 的版本保证线程安全
    // 绝对不能使用 localtime()，它会返回静态内部变量的指针，在多线程下必定 Data Race
    localtime_r(&seconds, &tm_time);

    if(showMicroseconds){
        int microseconds = static_cast<int>(microSecondsSinceEpoch_ % kMicroSecondsPerSecond);
        std::snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d.%06d",
            tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
            tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
            microseconds);
    }else{
        std::snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d",
            tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
            tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
    }
    return std::string(buf);
}