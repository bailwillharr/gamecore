#pragma once

#include <format>

#include <vec2.hpp>
#include <vec3.hpp>
#include <ext/quaternion_float.hpp>

template <>
struct std::formatter<glm::vec2> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const glm::vec2& vec, std::format_context& ctx) const { return std::format_to(ctx.out(), "[{}, {}]", vec.x, vec.y); }
};

template <>
struct std::formatter<glm::vec3> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const glm::vec3& vec, std::format_context& ctx) const { return std::format_to(ctx.out(), "[{}, {}, {}]", vec.x, vec.y, vec.z); }
};

template <>
struct std::formatter<glm::quat> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const glm::quat& vec, std::format_context& ctx) const { return std::format_to(ctx.out(), "[{}, {}, {}, {}]", vec.x, vec.y, vec.z, vec.w); }
};
