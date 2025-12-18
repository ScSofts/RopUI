#ifndef _ROPUI_LOG_H
#define _ROPUI_LOG_H

#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <mutex>
#include <cstdint>

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    NONE,
};

enum LogOption : uint32_t {
    LOG_OPT_NONE     = 0,
    LOG_OPT_TIME     = 1 << 0,
    LOG_OPT_LEVEL    = 1 << 1,
    LOG_OPT_FILE     = 1 << 2,
    LOG_OPT_LINE     = 1 << 3,
    LOG_OPT_FUNCTION = 1 << 4,
    LOG_OPT_COLOR    = 1 << 5,
};

inline LogOption operator|(LogOption a, LogOption b) {
    return static_cast<LogOption>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

class logger {
public:
    static void setOptions(uint32_t opts) {
        options_ = opts;
    }

    static void setOutput(FILE* fp) {
        output_ = fp ? fp : stdout;
    }

    static void setMinLevel(LogLevel level) {
        min_level_ = level;
    }

    static void logImpl(LogLevel level,
                        const char* file,
                        int line,
                        const char* func,
                        const char* fmt,
                        ...)
    {
        if (level < min_level_) return;

        char msg_buf[1024];

        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
        va_end(ap);

        std::lock_guard<std::mutex> lock(mutex_);

        if (options_ & LOG_OPT_COLOR) {
            std::fprintf(output_, "%s", levelToColor(level));
        }

        if (options_ & LOG_OPT_LEVEL) {
            std::fprintf(output_, "[%s] ", levelToString(level));
        }

        if (options_ & LOG_OPT_COLOR) {
            std::fprintf(output_, "%s", levelToColor(LogLevel::NONE));
        }

        if (options_ & LOG_OPT_FILE) {
            std::fprintf(output_, "%s", file);
            if (options_ & LOG_OPT_LINE) {
                std::fprintf(output_, ":%d", line);
            }
            std::fprintf(output_, " ");
        }

        if (options_ & LOG_OPT_FUNCTION) {
            std::fprintf(output_, "(%s) ", func);
        }

        if (options_ & LOG_OPT_TIME) {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            std::fprintf(output_,
                         "[%04d-%02d-%02d %02d:%02d:%02d] ",
                         tm.tm_year + 1900,
                         tm.tm_mon + 1,
                         tm.tm_mday,
                         tm.tm_hour,
                         tm.tm_min,
                         tm.tm_sec);
        }

        std::fprintf(output_, "%s", msg_buf);

        if (options_ & LOG_OPT_COLOR) {
            std::fprintf(output_, "\033[0m");
        }

        std::fprintf(output_, "\n");
        std::fflush(output_);
    }

private:
    static const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default:              return "UNKNOWN";
        }
    }

    static const char* levelToColor(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "\033[36m";
            case LogLevel::INFO:  return "\033[32m";
            case LogLevel::WARN:  return "\033[33m";
            case LogLevel::ERROR: return "\033[31m";
            case LogLevel::FATAL: return "\033[1;31m";
            default:              return "\033[0m";
        }
    }

private:
    static inline uint32_t options_ =
        LOG_OPT_LEVEL |
        LOG_OPT_FILE  |
        LOG_OPT_LINE  |
        LOG_OPT_COLOR ;

    static inline FILE* output_ = stdout;
    static inline LogLevel min_level_ = LogLevel::INFO;
    static inline std::mutex mutex_;
};

#define _LOG_DEBUG(fmt, ...) \
    logger::logImpl(LogLevel::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define _LOG_INFO(fmt, ...) \
    logger::logImpl(LogLevel::INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define _LOG_WARN(fmt, ...) \
    logger::logImpl(LogLevel::WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define _LOG_ERROR(fmt, ...) \
    logger::logImpl(LogLevel::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define _LOG_FATAL(fmt, ...) \
    logger::logImpl(LogLevel::FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG(level) _LOG_##level

#endif // _ROPUI_LOG_H
