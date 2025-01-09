#pragma once

#include <cstdlib>

#include <format>
#include <string_view>

#include "gamecore/gc_logger.h"

namespace gc {

/* Aborts the program and logs an error message */
/* Should only be used if the error is absolutely non recoverable */
template <typename... Args>
[[noreturn]] inline void abortGame(std::format_string<Args...> fmt, Args&&... args)
{
    GC_CRITICAL(fmt, std::forward<Args>(args)...);
    std::abort();
}

} // namespace gc