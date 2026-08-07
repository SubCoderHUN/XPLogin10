#include "xplogin/Logging.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xplogin {
namespace {

std::mutex g_mutex;
std::string g_path;
LogLevel g_level = LogLevel::Info;
Log::Sink g_sink = nullptr;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERR";
        case LogLevel::Info: return "INF";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Off: break;
    }
    return "---";
}

std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmValue{};
#ifdef _WIN32
    localtime_s(&tmValue, &now);
#else
    localtime_r(&now, &tmValue);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmValue);
    return buffer;
}

const char* BaseName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* last = slash > backslash ? slash : backslash;
    return last ? last + 1 : path;
}

} // namespace

void Log::Configure(const std::string& path, LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    g_level = level;
}

LogLevel Log::Level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

void Log::SetSink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = sink;
}

void Log::Write(LogLevel level, const char* file, int line, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_level == LogLevel::Off || level > g_level) {
            return;
        }
    }

    char message[1024];
    va_list args;
    va_start(args, fmt);
#ifdef _WIN32
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
#else
    vsnprintf(message, sizeof(message), fmt, args);
#endif
    va_end(args);

    char line_buffer[1280];
    snprintf(line_buffer, sizeof(line_buffer), "%s [%s] %s:%d %s\n",
             Timestamp().c_str(), LevelName(level), BaseName(file), line, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(level, line_buffer);
        return;
    }

#ifdef _WIN32
    OutputDebugStringA(line_buffer);
#endif
    if (!g_path.empty()) {
        FILE* file_handle = nullptr;
#ifdef _WIN32
        if (fopen_s(&file_handle, g_path.c_str(), "a") != 0) {
            file_handle = nullptr;
        }
#else
        file_handle = std::fopen(g_path.c_str(), "a");
#endif
        if (file_handle) {
            std::fputs(line_buffer, file_handle);
            std::fclose(file_handle);
        }
    }
}

void Log::Flush() {}

} // namespace xplogin
