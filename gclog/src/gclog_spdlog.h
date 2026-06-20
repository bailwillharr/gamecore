#pragma once

#include <memory>
#include <string_view>

#include "gclog/gclog.h"

namespace spdlog {
class logger; // forward-dec
}

namespace gclog {

/* Create with createLoggerSpdlog() */
class LoggerSpdlog final : public Logger {
    std::unique_ptr<spdlog::logger> m_spdlogger;
    bool m_log_file_set = false;

public:
    LoggerSpdlog();
    LoggerSpdlog(const LoggerSpdlog&) = delete;

    ~LoggerSpdlog() override;

    LoggerSpdlog& operator=(const LoggerSpdlog&) = delete;

    void setLogFile(const std::filesystem::path& file) override;

    void log(std::string_view message, LogLevel level) override;
};

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog();

} // namespace gclog
