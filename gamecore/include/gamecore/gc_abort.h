#pragma once

#include <cstdlib>

#include <format>

#include <SDL3/SDL_messagebox.h>

#include "gamecore/gc_logger.h"

namespace gc {

/* Aborts the program and logs an error message */
/* Should only be used if the error is absolutely non recoverable */
template <typename... Args>
[[noreturn]] inline void abortGame(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string formatted = std::format(fmt, std::forward<Args>(args)...);
    GC_CRITICAL("{}", formatted);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gamecore critical error", formatted.c_str(), nullptr);
    std::abort();
}

} // namespace gc
