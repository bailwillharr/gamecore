#include "gamecore/gc_logger_null.h"

#include <string_view>

namespace gc {

void LoggerNull::log(std::string_view message, LogLevel level)
{
    (void)message;
    (void)level;
    // do nothing
}

} // namespace gc
