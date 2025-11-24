#include "gamecore/gc_units.h"

#include <cmath>

#include <array>
#include <string>
#include <string_view>
#include <format>

#include "gamecore/gc_assert.h"

namespace gc {

std::string bytesToHumanReadable(std::uint64_t bytes)
{
    constexpr std::array units{"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    const int idx = bytes == 0 ? 0 : static_cast<int>(std::floor(std::log2(bytes) / 10.));
    GC_ASSERT(idx < static_cast<int>(units.size())); // UINT64_MAX => 16 EB
    const double value = static_cast<double>(bytes) / static_cast<double>(1ULL << (idx * 10));
    return std::format("{:.3g} {}", value, units[static_cast<unsigned>(idx)]);
}

} // namespace gc
