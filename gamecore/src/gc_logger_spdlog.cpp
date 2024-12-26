#include "gamecore/gc_logger_spdlog.h"

#include <vector>
#include <memory>
#include <string_view>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace gc {

LoggerSpdlog::LoggerSpdlog() : m_spdlogger(nullptr)
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.back()->set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread:%t] %v");
    m_spdlogger = std::make_unique<spdlog::logger>("gamecore", sinks.begin(), sinks.end());
    m_spdlogger->set_level(spdlog::level::trace);
    trace("Initialised LoggerSpdlog");
}

LoggerSpdlog::~LoggerSpdlog() { trace("Shutting down LoggerSpdlog"); }

void LoggerSpdlog::log(std::string_view message, LogLevel level)
{
    switch (level) {
        case LogLevel::LVL_TRACE:
            m_spdlogger->trace("{}", message);
            break;
        case LogLevel::LVL_DEBUG:
            m_spdlogger->debug("{}", message);
            break;
        case LogLevel::LVL_INFO:
            m_spdlogger->info("{}", message);
            break;
        case LogLevel::LVL_WARN:
            m_spdlogger->warn("{}", message);
            break;
        case LogLevel::LVL_ERROR:
            m_spdlogger->error("{}", message);
            break;
        case LogLevel::LVL_CRITICAL:
            m_spdlogger->critical("{}", message);
            break;
    }
}

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog() { return std::make_unique<LoggerSpdlog>(); }

} // namespace gc
