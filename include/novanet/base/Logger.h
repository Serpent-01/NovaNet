#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <unistd.h>

namespace novanet::base {

enum class LogLevel : int {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    SysErr,
    Fatal,
    SysFatal,
    Off,
};

class Logger {
public:
    using OutputFunction = std::function<void(const std::string&)>;
    using FlushFunction = std::function<void()>;

    Logger(LogLevel level, const char* file, int line)
        : level_(level),
          savedErrno_(errno) {
        stream_ << timestampNow() << " [" << levelToString(level_) << "] "
                << "pid=" << ::getpid() << " "
                << "tid=" << std::this_thread::get_id() << " "
                << baseName(file) << ":" << line << " | ";
    }

    ~Logger() noexcept {
        const bool fatal = isFatal(level_);

        try {
            if (level_ == LogLevel::SysErr || level_ == LogLevel::SysFatal) {
                stream_ << " errno=" << savedErrno_ << " ("
                        << std::error_code(savedErrno_, std::system_category())
                               .message()
                        << ")";
            }

            stream_ << '\n';
            write(stream_.str(), fatal || level_ == LogLevel::Error ||
                                     level_ == LogLevel::SysErr);
        } catch (...) {
            // Logging must never throw from a destructor.
        }

        if (fatal) {
            std::abort();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    template <typename T>
    Logger& operator<<(T&& value) {
        stream_ << std::forward<T>(value);
        return *this;
    }

    Logger& operator<<(std::ostream& (*manip)(std::ostream&)) {
        manip(stream_);
        return *this;
    }

    [[nodiscard]] static LogLevel logLevel() noexcept {
        return g_logLevel.load(std::memory_order_relaxed);
    }

    static void setLogLevel(LogLevel level) noexcept {
        g_logLevel.store(level, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool shouldLog(LogLevel level) noexcept {
        if (isFatal(level)) {
            return true;
        }

        const LogLevel threshold = logLevel();
        if (threshold == LogLevel::Off) {
            return false;
        }

        return static_cast<int>(level) >= static_cast<int>(threshold);
    }

    static void setOutput(OutputFunction output) {
        std::lock_guard<std::mutex> lock(configMutex());
        outputFunction() = output ? std::move(output) : defaultOutput;
    }

    static void setFlush(FlushFunction flush) {
        std::lock_guard<std::mutex> lock(configMutex());
        flushFunction() = flush ? std::move(flush) : defaultFlush;
    }

    [[nodiscard]] static const char* levelToString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Trace:
                return "TRACE";
            case LogLevel::Debug:
                return "DEBUG";
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
            case LogLevel::Off:
                return "OFF";
            default:
                return "UNKNOWN";
        }
    }

private:
    [[nodiscard]] static bool isFatal(LogLevel level) noexcept {
        return level == LogLevel::Fatal || level == LogLevel::SysFatal;
    }

    [[nodiscard]] static const char* baseName(const char* file) noexcept {
        if (file == nullptr) {
            return "";
        }

        const char* base = file;
        if (const char* slash = std::strrchr(file, '/')) {
            base = slash + 1;
        }
        if (const char* backslash = std::strrchr(file, '\\')) {
            if (backslash + 1 > base) {
                base = backslash + 1;
            }
        }

        return base;
    }

    [[nodiscard]] static std::string timestampNow() {
        using clock = std::chrono::system_clock;

        const auto now = clock::now();
        const auto sinceEpoch = now.time_since_epoch();
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(
                sinceEpoch - seconds);

        const std::time_t time = clock::to_time_t(now);
        std::tm localTime{};
        ::localtime_r(&time, &localTime);

        char date[32] = {};
        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &localTime);

        std::ostringstream out;
        out << date << "." << std::setw(6) << std::setfill('0')
            << micros.count();
        return out.str();
    }

    static void write(const std::string& message, bool flush) {
        std::lock_guard<std::mutex> lock(configMutex());
        outputFunction()(message);
        if (flush) {
            flushFunction()();
        }
    }

    static void defaultOutput(const std::string& message) {
        std::cerr << message;
    }

    static void defaultFlush() {
        std::cerr.flush();
    }

    [[nodiscard]] static OutputFunction& outputFunction() {
        static OutputFunction output = defaultOutput;
        return output;
    }

    [[nodiscard]] static FlushFunction& flushFunction() {
        static FlushFunction flush = defaultFlush;
        return flush;
    }

    [[nodiscard]] static std::mutex& configMutex() {
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

#define NOVANET_LOG(level)                                                     \
    if (!::novanet::base::Logger::shouldLog(::novanet::base::LogLevel::level)) \
    {                                                                          \
    }                                                                          \
    else                                                                       \
        ::novanet::base::Logger(::novanet::base::LogLevel::level, __FILE__,    \
                                __LINE__)

#define LOG_TRACE NOVANET_LOG(Trace)
#define LOG_DEBUG NOVANET_LOG(Debug)
#define LOG_INFO NOVANET_LOG(Info)
#define LOG_WARN NOVANET_LOG(Warn)
#define LOG_ERROR NOVANET_LOG(Error)
#define LOG_SYSERR NOVANET_LOG(SysErr)
#define LOG_FATAL NOVANET_LOG(Fatal)
#define LOG_SYSFATAL NOVANET_LOG(SysFatal)
