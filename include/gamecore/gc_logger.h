#pragma once

#include <string_view>

namespace gc {

/* 'ERROR' is defined as a macro in Windows.h so LVL_ is prepended */
enum class LogLevel { LVL_TRACE = 0, LVL_DEBUG = 1, LVL_INFO = 2, LVL_WARN = 3, LVL_ERROR = 4, LVL_CRITICAL = 5 };

class Logger {

public:
    virtual ~Logger() = 0;

    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

    static Logger& instance();

private:
    virtual void log(std::string_view message, LogLevel level);
};

} // namespace gc
