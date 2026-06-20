#pragma once

#include <atomic>
#include <string_view>
#include <format>
#include <filesystem>

/* Logger macros that cut TRACE and DEBUG from release builds */
#ifdef GC_LOG_TRACE
#define GC_TRACE(...) ::gclog::Logger::instance().trace(::std::format(__VA_ARGS__))
#else
#define GC_TRACE(...) (void)0
#endif
#ifdef GC_LOG_DEBUG
#define GC_DEBUG(...) ::gclog::Logger::instance().debug(::std::format(__VA_ARGS__))
#else
#define GC_DEBUG(...) (void)0
#endif
#define GC_INFO(...) ::gclog::Logger::instance().info(::std::format(__VA_ARGS__))
#define GC_WARN(...) ::gclog::Logger::instance().warn(::std::format(__VA_ARGS__))
#define GC_ERROR(...) ::gclog::Logger::instance().error(::std::format(__VA_ARGS__))
#define GC_CRITICAL(...) ::gclog::Logger::instance().critical(::std::format(__VA_ARGS__))

#define GC_WARN_ONCE(...)                                                 \
    do {                                                                  \
        static bool _gclog_warn_once_logged = false;                      \
        if (!_gclog_warn_once_logged) {                                   \
            ::gclog::Logger::instance().warn(::std::format(__VA_ARGS__)); \
            _gclog_warn_once_logged = true;                               \
        }                                                                 \
    } while (0)

#define GC_ERROR_ONCE(...)                                                 \
    do {                                                                   \
        static bool _gclog_warn_once_logged = false;                       \
        if (!_gclog_warn_once_logged) {                                    \
            ::gclog::Logger::instance().error(::std::format(__VA_ARGS__)); \
            _gclog_warn_once_logged = true;                                \
        }                                                                  \
    } while (0)

namespace gclog {

/* 'ERROR' is defined as a macro in Windows.h so LVL_ is prepended */
enum class LogLevel { LVL_TRACE = 0, LVL_DEBUG = 1, LVL_INFO = 2, LVL_WARN = 3, LVL_ERROR = 4, LVL_CRITICAL = 5 };

class Logger {
    std::atomic<int64_t> m_frame_number{-1}; // -1 means before game loop starts

public:
    virtual ~Logger();

    void incrementFrameNumber();
    int64_t getFrameNumber() const;

    // Must be called from main thread
    virtual void setLogFile(const std::filesystem::path&);

    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

    static Logger& instance();

private:
    virtual void log(std::string_view message, LogLevel level);
};

} // namespace gclog
