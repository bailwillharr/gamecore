#include "gamecore/gc_logger.h"

#include <string_view>
#include <memory>
#include <mutex>

#include "gamecore/gc_defines.h"
#if GC_LOGGER == GC_LOGGER_SPDLOG
#include "gamecore/gc_logger_spdlog.h"
#endif

namespace gc {

Logger::~Logger() {}

void Logger::trace(std::string_view message)
{
#ifdef GC_LOG_TRACE_DEBUG
    log(message, LogLevel::LVL_TRACE);
#else
    (void)message;
#endif
}

void Logger::debug(std::string_view message)
{
#ifdef GC_LOG_TRACE_DEBUG
    log(message, LogLevel::LVL_DEBUG);
#else
    (void)message;
#endif
}

void Logger::info(std::string_view message) { log(message, LogLevel::LVL_INFO); }

void Logger::warn(std::string_view message) { log(message, LogLevel::LVL_WARN); }

void Logger::error(std::string_view message) { log(message, LogLevel::LVL_ERROR); }

void Logger::critical(std::string_view message) { log(message, LogLevel::LVL_CRITICAL); }

Logger& Logger::instance()
{
#if GC_LOGGER == GC_LOGGER_SPDLOG
    static LoggerSpdlog logger;
#else
    static Logger logger;
#endif
    return logger;
}

// default logger does nothing
void Logger::log(std::string_view message, LogLevel level)
{
    (void)message;
    (void)level;
}

} // namespace gc
