#include "gclog_spdlog.h"

#include <vector>
#include <memory>
#include <string_view>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#endif

namespace gclog {

LoggerSpdlog::LoggerSpdlog() : m_spdlogger(nullptr)
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.back()->set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread:%t] %v");
#ifdef _WIN32
    sinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
    sinks.back()->set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread:%t] %v");
#endif
    m_spdlogger = std::make_unique<spdlog::logger>("gamecore", sinks.begin(), sinks.end());
    m_spdlogger->set_level(spdlog::level::trace);
}

LoggerSpdlog::~LoggerSpdlog() { GC_TRACE("Destroying LoggerSpdlog..."); }

void LoggerSpdlog::setLogFile(const std::filesystem::path& file)
{
    if (!m_log_file_set) {
        m_spdlogger->sinks().emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string()));
        m_log_file_set = true;
    }
}

void LoggerSpdlog::log(std::string_view message, LogLevel level)
{
    const auto formatted_message = std::format("[frame:{}] {}", getFrameNumber(), message);
    switch (level) {
    case LogLevel::LVL_TRACE:
        m_spdlogger->trace("{}", formatted_message);
        break;
    case LogLevel::LVL_DEBUG:
        m_spdlogger->debug("{}", formatted_message);
        break;
    case LogLevel::LVL_INFO:
        m_spdlogger->info("{}", formatted_message);
        break;
    case LogLevel::LVL_WARN:
        m_spdlogger->warn("{}", formatted_message);
        break;
    case LogLevel::LVL_ERROR:
        m_spdlogger->error("{}", formatted_message);
        break;
    case LogLevel::LVL_CRITICAL:
        m_spdlogger->critical("{}", formatted_message);
        break;
    }
}

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog() { return std::make_unique<LoggerSpdlog>(); }

} // namespace gclog
