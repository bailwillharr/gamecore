#pragma once

#include <memory>
#include <string_view>

#include "gamecore/gc_logger.h"

namespace spdlog {
class logger; // forward-dec
}

namespace gc {

/* Create with createLoggerSpdlog() */
class LoggerSpdlog final : public Logger {
    std::unique_ptr<spdlog::logger> m_spdlogger;

public:
    LoggerSpdlog();
    LoggerSpdlog(const LoggerSpdlog&) = delete;

    ~LoggerSpdlog();

    LoggerSpdlog& operator=(const LoggerSpdlog&) = delete;

    void log(std::string_view message, LogLevel level) override;
};

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog();

} // namespace gc
