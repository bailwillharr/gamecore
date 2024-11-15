#pragma once

#include <string_view>

namespace gc {

enum class LogLevel { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, CRITICAL = 5 };

class Logger {

public:
    virtual ~Logger() = 0;

    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

private:
    virtual void log(std::string_view message, LogLevel level) = 0;
};

} // namespace gc
