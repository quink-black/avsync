#pragma once

#include <cstdio>
#include <cstdarg>
#include <string>

namespace avsync {

enum class LogLevel {
    Debug = 0,
    Info,
    Warn,
    Error
};

// Simple cross-platform logger (thread-safe is not required for phase 1)
class Log {
public:
    static void SetLevel(LogLevel level) { GetLevel() = level; }

    static void Debug(const char* fmt, ...) {
        if (GetLevel() > LogLevel::Debug) return;
        va_list args;
        va_start(args, fmt);
        Print("[DEBUG] ", fmt, args);
        va_end(args);
    }

    static void Info(const char* fmt, ...) {
        if (GetLevel() > LogLevel::Info) return;
        va_list args;
        va_start(args, fmt);
        Print("[INFO]  ", fmt, args);
        va_end(args);
    }

    static void Warn(const char* fmt, ...) {
        if (GetLevel() > LogLevel::Warn) return;
        va_list args;
        va_start(args, fmt);
        Print("[WARN]  ", fmt, args);
        va_end(args);
    }

    static void Error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        Print("[ERROR] ", fmt, args);
        va_end(args);
    }

private:
    static LogLevel& GetLevel() {
        static LogLevel level = LogLevel::Info;
        return level;
    }

    static void Print(const char* prefix, const char* fmt, va_list args) {
        std::fprintf(stderr, "%s", prefix);
        std::vfprintf(stderr, fmt, args);
        std::fprintf(stderr, "\n");
    }
};

}  // namespace avsync
