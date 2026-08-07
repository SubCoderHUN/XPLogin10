// XPLogin10 - logging.
//
// Inside LogonUI there is no console and no debugger attached, so the log file
// is usually the only evidence of what happened. Writes are best effort and
// never throw: a failure to log must never fail a logon.
#pragma once

#include <cstdarg>
#include <string>

namespace xplogin {

enum class LogLevel {
    Off = 0,
    Error = 1,
    Info = 2,
    Debug = 3,
};

class Log {
public:
    // Path is UTF-8. An empty path disables file output (OutputDebugString on
    // Windows still receives everything).
    static void Configure(const std::string& path, LogLevel level);
    static void Write(LogLevel level, const char* file, int line, const char* fmt, ...);
    static LogLevel Level();
    static void Flush();

    // Test hook: capture instead of write.
    using Sink = void (*)(LogLevel level, const std::string& line);
    static void SetSink(Sink sink);
};

} // namespace xplogin

#define XPLOG_ERROR(...) \
    ::xplogin::Log::Write(::xplogin::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
#define XPLOG_INFO(...) \
    ::xplogin::Log::Write(::xplogin::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define XPLOG_DEBUG(...) \
    ::xplogin::Log::Write(::xplogin::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
