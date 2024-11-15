#pragma once

#include <string_view>

#include "gamecore/gc_logger.h"

namespace gc {

class LoggerNull : public Logger {
public:
    void log(std::string_view message, LogLevel level) override;
};

} // namespace gc
