#pragma once

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace novanet::base {

enum class LogLevel { Info = 0, Warn, Error, SysErr, Fatal, SysFatal };

class Logger {
public:
    static LogLevel logLevel() noexcept {
        return g_logLevel.load(std::memory_order_relaxed);
    }

    static void setLogLevel(LogLevel level) noexcept {
        g_logLevel.store(level, std::memory_order_relaxed);
    }

    Logger(LogLevel level, const char* file, int line)
        : level_(level), savedErrno_(errno) {
        stream_ << "[" << levelToString(level_) << "] " << file << ":" << line
                << " | ";
    }

    ~Logger() noexcept {
        try {
            if (level_ == LogLevel::SysErr || level_ == LogLevel::SysFatal) {
                stream_ << " (system error: "
                        << std::error_code(savedErrno_, std::system_category())
                               .message()
                        << ")";
            }

            stream_ << '\n';

            {
                std::lock_guard<std::mutex> lock(outputMutex());
                std::cerr << stream_.str();

                if (level_ == LogLevel::Fatal || level_ == LogLevel::SysFatal) {
                    std::cerr.flush();
                }
            }

            if (level_ == LogLevel::Fatal || level_ == LogLevel::SysFatal) {
                std::abort();
            }
        } catch (...) {
            // 析构函数绝不能抛异常。
        }
    }

    template <typename T>
    Logger& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

private:
    static const char* levelToString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Info:
                return "INFO";
            case LogLevel::Warn:
                return "WARN";
            case LogLevel::Error:
                return "ERROR";
            case LogLevel::SysErr:
                return "SYSERR";
            case LogLevel::Fatal:
                return "FATAL";
            case LogLevel::SysFatal:
                return "SYSFATAL";
            default:
                return "UNKNOWN";
        }
    }

    static std::mutex& outputMutex() {
        static std::mutex mutex;
        return mutex;
    }

private:
    inline static std::atomic<LogLevel> g_logLevel{LogLevel::Warn};

    LogLevel level_;
    int savedErrno_;
    std::ostringstream stream_;
};

}  // namespace novanet::base

#define LOG_INFO                                                             \
    if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Info) { \
    } else                                                                   \
        novanet::base::Logger(novanet::base::LogLevel::Info, __FILE__, __LINE__)

#define LOG_WARN                                                             \
    if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Warn) { \
    } else                                                                   \
        novanet::base::Logger(novanet::base::LogLevel::Warn, __FILE__, __LINE__)

#define LOG_ERROR                                                             \
    if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Error) { \
    } else                                                                    \
        novanet::base::Logger(novanet::base::LogLevel::Error, __FILE__,       \
                              __LINE__)

#define LOG_SYSERR                                                             \
    if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::SysErr) { \
    } else                                                                     \
        novanet::base::Logger(novanet::base::LogLevel::SysErr, __FILE__,       \
                              __LINE__)

#define LOG_FATAL \
    novanet::base::Logger(novanet::base::LogLevel::Fatal, __FILE__, __LINE__)

#define LOG_SYSFATAL \
    novanet::base::Logger(novanet::base::LogLevel::SysFatal, __FILE__, __LINE__)