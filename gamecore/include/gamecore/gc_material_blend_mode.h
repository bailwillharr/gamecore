#pragma once

#include <cstdint>

namespace gc {

// How a material's alpha is used. The alpha is the base color texture's, or the base color constant's if there is no texture.
enum class MaterialBlendMode : uint32_t {
    NONE = 0,        // opaque. The alpha is ignored
    ALPHA_TEST = 1,  // cut out: opaque where the alpha is at least the material's alpha cutoff, and nothing at all elsewhere
    ALPHA_BLEND = 2, // see-through: mixed with what is behind it by the alpha. Drawn after everything else, furthest first, and
                     // doesn't write depth, so see-through surfaces that pass through each other can look wrong
};
constexpr uint32_t MATERIAL_BLEND_MODE_COUNT = 3;

} // namespace gc
