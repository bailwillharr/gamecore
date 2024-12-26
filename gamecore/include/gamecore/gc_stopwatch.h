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

inline void tock(Tick tick)
{
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tick.second);
    GC_DEBUG("Stopwatch '{}' took {}", tick.first, duration);
}

} // namespace gc