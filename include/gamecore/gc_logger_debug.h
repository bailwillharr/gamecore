#pragma once

#include "gamecore/gc_logger.h"

namespace gc {

class LoggerDebug : public Logger {
public:
    void log(const char* message, LogLevel level) override;
};

} // namespace gc
