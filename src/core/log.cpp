#include "core/log.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

static FILE* s_logFile = nullptr;

static const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "???";
}

static const char* stripPath(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

void Log::init(const char* logFilePath) {
    if (logFilePath) {
        s_logFile = fopen(logFilePath, "w");
        if (!s_logFile) {
            fprintf(stderr, "[WARN] Could not open log file: %s\n", logFilePath);
        }
    }
}

void Log::shutdown() {
    if (s_logFile) {
        fclose(s_logFile);
        s_logFile = nullptr;
    }
}

void Log::log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);

    const char* filename = stripPath(file);
    const char* levelStr = levelToString(level);

    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    // Skip console output on Windows Release builds — Win32 console I/O is
    // extremely slow (1-5ms per call) and causes visible microstutters.
#if !defined(NDEBUG) || !defined(_WIN32)
    FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
    fprintf(out, "[%s] %s %s:%d: %s\n", levelStr, timeBuf, filename, line, msgBuf);
#endif

    if (s_logFile) {
        fprintf(s_logFile, "[%s] %s %s:%d: %s\n", levelStr, timeBuf, filename, line, msgBuf);
        fflush(s_logFile);
    }
}
