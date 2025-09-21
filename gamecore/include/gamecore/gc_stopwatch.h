#pragma once

#include <chrono>
#include <tuple>

#include "gamecore/gc_logger.h"

namespace gc {

using Tick = std::pair<std::string, std::chrono::steady_clock::time_point>;

inline Tick tick(std::string name)
{
    // stores time point and some name
    return std::make_pair(name, std::chrono::steady_clock::now());
}

/* Logs the time taken and returns the time taken in seconds */
inline double tock(Tick tick)
{
    using namespace std::literals;

    const double duration_sec = (std::chrono::steady_clock::now() - tick.second) / 1.0s;
    GC_TRACE("Stopwatch '{}' took {} ms", tick.first, duration_sec * 1000.0);
    return duration_sec;
}

} // namespace gc