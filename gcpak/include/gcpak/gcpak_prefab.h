#pragma once

#include <cstdint>

#include <limits>

namespace gcpak {

// An instantiatable entity tree.
// Designed to be efficiently loaded into the world.
// Contains a packed list component declarations. (component declarations are not neccesarily the same size)
// Order of entities in the list must match hierarchy order (no children before parent)
// Root entity must be index zero.
// A new entity is declared with a TransformComponent declaration. TransformComponent == ENTITY BEGIN MARKER
// No other component type can appear before the first TransformComponent
// First field in all component declarations is PrefabComponentType.

// Example data structure:
// 0000-0003 PrefabComponentType = TRANSFORM (NEW ENTITY)
// 0004-0083 Serialised TransformComponent
// 0084-0087 PrefabComponentType = RENDERABLE
// 0088-008F Serialised RenderableComponent
// 0090-0093 PrefabComponentType = TRANSFORM (NEW ENTITY)
// 0094-0113 Serialised TransformComponent
// 0114-0117 PrefabComponentType = RENDERABLE
// 0118-011F Serialised LightComponent

static_assert(std::numeric_limits<float>::is_iec559);
static_assert(sizeof(float) == 4);

enum class PrefabComponentType : uint32_t {
    TRANSFORM = 0,
    RENDERABLE = 1,
    CAMERA = 2,
    LIGHT = 3,
};

} // namespace gcpak
