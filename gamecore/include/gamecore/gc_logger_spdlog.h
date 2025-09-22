#pragma once

#include <atomic>
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
    std::atomic<int64_t> m_frame_number; // -1 means before game loop starts

public:
    LoggerSpdlog();
    LoggerSpdlog(const LoggerSpdlog&) = delete;

    ~LoggerSpdlog() override;

    LoggerSpdlog& operator=(const LoggerSpdlog&) = delete;

    void incrementFrameNumber() override;

    void log(std::string_view message, LogLevel level) override;
};

std::unique_ptr<LoggerSpdlog> createLoggerSpdlog();

} // namespace gc
