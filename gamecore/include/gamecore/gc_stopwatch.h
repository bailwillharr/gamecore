#pragma once

#include <chrono>

#include "gamecore/gc_logger.h"

namespace gc {

class Stopwatch; // forward-dec

} // namespace gc

template <>
struct std::formatter<gc::Stopwatch>; // forward-dec

namespace gc {

class Stopwatch {
    friend struct std::formatter<gc::Stopwatch>;

    const std::chrono::steady_clock::time_point m_start;

public:
    Stopwatch() : m_start(std::chrono::steady_clock::now()) {}
};

} // namespace gc

template <>
struct std::formatter<gc::Stopwatch> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const gc::Stopwatch& sw, std::format_context& ctx) const
    {
        using namespace std::literals;
        const double duration_sec = (std::chrono::steady_clock::now() - sw.m_start) / 1.0ms;
        return std::format_to(ctx.out(), "{:.3} ms", duration_sec);
    }
};
