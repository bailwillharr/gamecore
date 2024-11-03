#pragma once

namespace gc {

enum class LogLevel { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, CRITICAL = 5 };

class Logger {

public:
    virtual ~Logger() = 0;
    virtual void log(const char* message, LogLevel level) = 0;
    void trace(const char* message);
    void debug(const char* message);
    void info(const char* message);
    void warn(const char* message);
    void error(const char* message);
    void critical(const char* message);
};

} // namespace gc
