#include "gamecore/gc_units.h"

#include <cmath>

#include <string>
#include <format>

#include "gamecore/gc_assert.h"

namespace gc {

std::string bytesToHumanReadable(std::uint64_t bytes)
{
    const std::string units{"BKMGTPE"};
    const int idx = static_cast<int>(std::floor(std::log2(bytes) / 10.));
    GC_ASSERT(idx < static_cast<int>(units.size())); // UINT64_MAX => 16 EB
    return std::format("{}{}", static_cast<double>(bytes >> (idx * 10)), units[static_cast<unsigned>(idx)]);
}

} // namespace gc
