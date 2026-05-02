#pragma once

#include <cstdio>
#include <cstdarg>

enum class LogLevel : int {
    Info = 0,
    Warn = 1,
    Error = 2
};

namespace Log {
    void init(const char* logFilePath = nullptr);
    void shutdown();
    void log(LogLevel level, const char* file, int line, const char* fmt, ...);
}

#define LOG_INFO(fmt, ...)  Log::log(LogLevel::Info,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Log::log(LogLevel::Warn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Log::log(LogLevel::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
