#include "gamecore/gc_logger_spdlog.h"

#include <vector>
#include <memory>
#include <thread>
#include <string_view>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "gamecore/gc_abort.h"

namespace gc {

LoggerSpdlog::LoggerSpdlog() : m_spdlogger(nullptr), m_frame_number(-1), m_main_thread_id(std::this_thread::get_id())
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.back()->set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread:%t] %v");
    m_spdlogger = std::make_unique<spdlog::logger>("gamecore", sinks.begin(), sinks.end());
    m_spdlogger->set_level(spdlog::level::trace);
    GC_TRACE("Initialised LoggerSpdlog");
}

LoggerSpdlog::~LoggerSpdlog() { GC_TRACE("Destroying LoggerSpdlog..."); }

void LoggerSpdlog::incrementFrameNumber()
{
    if (std::this_thread::get_id() != m_main_thread_id) {
        gc::abortGame("Attempt to call LoggerSpdlog::incrementFrameNumber() from another thread!");
    }
    m_frame_number.fetch_add(1LL, std::memory_order_relaxed);
}

void LoggerSpdlog::log(std::string_view message, LogLevel level)
{
    const auto formatted_message = std::format("[frame:{}] {}", m_frame_number.load(std::memory_order_relaxed), message);
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

} // namespace gc
