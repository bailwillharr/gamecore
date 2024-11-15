#include "gamecore/gc_logger.h"

#include <string_view>

namespace gc {

Logger::~Logger() {}

void Logger::trace(std::string_view message)
{
#ifdef GC_LOG_TRACE_DEBUG
    log(message, LogLevel::TRACE);
#else
    (void)message;
#endif
}

void Logger::debug(std::string_view message)
{
#ifdef GC_LOG_TRACE_DEBUG
    log(message, LogLevel::DEBUG);
#else
    (void)message;
#endif
}

void Logger::info(std::string_view message) { log(message, LogLevel::INFO); }

void Logger::warn(std::string_view message) { log(message, LogLevel::WARN); }

void Logger::error(std::string_view message) { log(message, LogLevel::ERROR); }

void Logger::critical(std::string_view message) { log(message, LogLevel::CRITICAL); }

} // namespace gc
