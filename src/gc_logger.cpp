#include "gamecore/gc_logger.h"

namespace gc {

Logger::~Logger() {}

void Logger::trace(const char* message) { log(message, LogLevel::TRACE); }
void Logger::debug(const char* message) { log(message, LogLevel::DEBUG); }
void Logger::info(const char* message) { log(message, LogLevel::INFO); }
void Logger::warn(const char* message) { log(message, LogLevel::WARN); }
void Logger::error(const char* message) { log(message, LogLevel::ERROR); }
void Logger::critical(const char* message) { log(message, LogLevel::CRITICAL); }

} // namespace gc
