#include "gamecore/gc_logger_debug.h"

#include <cstdio>

namespace gc {

void LoggerDebug::log(const char* message, LogLevel level)
{
    (void)level;
    puts(message);
}

} // namespace gc
