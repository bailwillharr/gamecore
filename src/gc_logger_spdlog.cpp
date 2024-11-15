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
}

LoggerSpdlog::~LoggerSpdlog() {}

void LoggerSpdlog::log(std::string_view message, LogLevel level)
{
    switch (level) {
        case LogLevel::TRACE:
            m_spdlogger->trace("{}", message);
            break;
        case LogLevel::DEBUG:
            m_spdlogger->debug("{}", message);
            break;
        case LogLevel::INFO:
            m_spdlogger->info("{}", message);
            break;
        case LogLevel::WARN:
            m_spdlogger->warn("{}", message);
            break;
        case LogLevel::ERROR:
            m_spdlogger->error("{}", message);
            break;
        case LogLevel::CRITICAL:
            m_spdlogger->critical("{}", message);
            break;
    }
}

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog() { return std::make_unique<LoggerSpdlog>(); }

} // namespace gc
