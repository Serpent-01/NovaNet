#pragma once

#include <iostream>
#include <sstream>
#include <cstdlib>
#include <cerrno>
#include <system_error>

namespace novanet::base {

// TODO A2(P2): 将 LogLevel 改成 enum class，提升类型安全性，防止命名空间污染
enum class LogLevel {
    Info = 0,
    Warn,
    Error,
    SysErr,
    Fatal,
    SysFatal
};

class Logger {
public:
    // TODO A1(P1): 增加全局日志级别开关 (利用 C++17 inline 静态变量)
    // 默认设置为 Warn，彻底屏蔽热路径上的 INFO 日志，避免性能退化
    inline static LogLevel g_logLevel = LogLevel::Warn;

    static LogLevel logLevel() { return g_logLevel; }
    static void setLogLevel(LogLevel level) { g_logLevel = level; }

    Logger(LogLevel level, const char* file, int line) : level_(level) {
        savedErrno_ = errno; // 立即保存 errno，防止被后续操作覆盖
        stream_ << "[" << levelToString(level) << "] " << file << ":" << line << " | ";
    }

    // TODO A3(P2): 析构函数显式标记 noexcept，防止在栈展开期间抛出异常导致进程强制终止 (std::terminate)
    ~Logger() noexcept {
        try {
            // 保留 SYSERR 和 SYSFATAL 的 errno 解析
            if (level_ == LogLevel::SysErr || level_ == LogLevel::SysFatal) {
                stream_ << " (系统原由: " 
                        << std::error_code(savedErrno_, std::system_category()).message() 
                        << ")";
            }
            stream_ << "\n";
            
            std::cerr << stream_.str();
            
            // TODO A1(P1): 非 fatal 日志不再强制 flush()
            // 减少一次沉重的系统调用开销，将写入缓冲的权力交还给操作系统
            if (level_ == LogLevel::Fatal || level_ == LogLevel::SysFatal) {
                std::cerr.flush();
                std::abort(); // 致命错误触发程序崩溃
            }
        } catch (...) {
            // 吞掉所有可能的异常（如内存不足导致的 std::bad_alloc）
            // 确保没有任何异常能从析构函数逃逸
        }
    }

    template<typename T>
    Logger& operator<<(const T& val) {
        stream_ << val;
        return *this;
    }

private:
    LogLevel level_;
    int savedErrno_;
    std::ostringstream stream_;

    const char* levelToString(LogLevel level) noexcept {
        switch(level) {
            case LogLevel::Info:     return "INFO";
            case LogLevel::Warn:     return "WARN";
            case LogLevel::Error:    return "ERROR";
            case LogLevel::Fatal:    return "FATAL";
            case LogLevel::SysErr:   return "SYSERR";
            case LogLevel::SysFatal: return "SYSFATAL";
            default:                 return "UNKNOWN";
        }
    }
};

} // namespace novanet::base

// ==========================================
// 暴露给业务代码的宏：宏级“短路”机制 (Short-circuiting)
// ==========================================
// 核心优化思路 (A1)：如果 logLevel() > 目标级别，直接拦截。
// 这样就不会构造临时的 Logger 对象，不会创建沉重的 std::ostringstream，
// 也不会执行任何 << 拼接运算，实现了【被屏蔽级别的日志做到真正的零开销 (Zero-overhead)】。
// 使用 if() {} else 的语法能完美兼容任何作用域，防止 Dangling Else 悬挂问题。

#define LOG_INFO   if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Info) {} else novanet::base::Logger(novanet::base::LogLevel::Info,   __FILE__, __LINE__)
#define LOG_WARN   if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Warn) {} else novanet::base::Logger(novanet::base::LogLevel::Warn,   __FILE__, __LINE__)
#define LOG_ERROR  if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::Error) {} else novanet::base::Logger(novanet::base::LogLevel::Error,  __FILE__, __LINE__)
#define LOG_SYSERR if (novanet::base::Logger::logLevel() > novanet::base::LogLevel::SysErr) {} else novanet::base::Logger(novanet::base::LogLevel::SysErr, __FILE__, __LINE__)

// FATAL 级别永远不被屏蔽，一定会执行
#define LOG_FATAL    novanet::base::Logger(novanet::base::LogLevel::Fatal,    __FILE__, __LINE__)
#define LOG_SYSFATAL novanet::base::Logger(novanet::base::LogLevel::SysFatal, __FILE__, __LINE__)