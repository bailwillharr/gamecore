#pragma once

#include <string_view>
#include <format>

/* Logger macros that cut TRACE and DEBUG from release builds */
#ifdef GC_LOG_TRACE_DEBUG
#define GC_TRACE(...) ::gc::Logger::instance().trace(::std::format(__VA_ARGS__))
#define GC_DEBUG(...) ::gc::Logger::instance().debug(::std::format(__VA_ARGS__))
#else
#define GC_TRACE(...) (void)0
#define GC_DEBUG(...) (void)0
#endif
#define GC_INFO(...) ::gc::Logger::instance().info(::std::format(__VA_ARGS__))
#define GC_WARN(...) ::gc::Logger::instance().warn(::std::format(__VA_ARGS__))
#define GC_ERROR(...) ::gc::Logger::instance().error(::std::format(__VA_ARGS__))
#define GC_CRITICAL(...) ::gc::Logger::instance().critical(::std::format(__VA_ARGS__))

namespace gc {

/* 'ERROR' is defined as a macro in Windows.h so LVL_ is prepended */
enum class LogLevel { LVL_TRACE = 0, LVL_DEBUG = 1, LVL_INFO = 2, LVL_WARN = 3, LVL_ERROR = 4, LVL_CRITICAL = 5 };

class Logger {

public:
    virtual ~Logger();

    /* only logs if GC_LOG_TRACE_DEBUG is defined */
    void trace(std::string_view message);
    /* only logs if GC_LOG_TRACE_DEBUG is defined */
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

    static Logger& instance();

private:
    virtual void log(std::string_view message, LogLevel level);
};

} // namespace gc
