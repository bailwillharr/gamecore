#pragma once

#include <cstddef>
#include <cstdint>

#include <limits>

namespace gcpak {

// An instantiatable tree (or several trees) of entities. Anything from a single prop up to an entire game world.
// Designed to be efficiently loaded into the world.
//
// A prefab is a packed list of component declarations. Every declaration is:
//   uint32_t name_hash   crc32 of the component's name, e.g. "RenderableComponent". Same hash as an asset ID.
//   uint32_t size        number of bytes of component data that follow
//   uint8_t  data[size]  the serialised component
//
// Component types aren't known to the file format. The engine and the game give every component type a name, and the loader looks
// components up by that name at runtime. The size allows a loader to skip over a component it doesn't know about.
//
// A new entity is declared with a TransformComponent declaration. TransformComponent == ENTITY BEGIN MARKER
// Its data is the index of the entity's parent (uint32_t), followed by the serialised TransformComponent.
// The component declarations that follow, up to the next TransformComponent, belong to that entity.
// No other component type can appear before the first TransformComponent.
//
// Entities are indexed in the order they are declared. Order of entities must match hierarchy order (no children before parent),
// so a parent index is always less than the entity's own index.
// A parent index of PREFAB_NO_PARENT makes the entity a root of the prefab. A prefab has at least one root and can have many.
//
// Prefabs don't contain resources. Components refer to meshes, materials etc. by asset ID, which are resolved when they are needed.
//
// Example data structure:
// 0000-0003 name_hash = "TransformComponent" (NEW ENTITY, index 0)
// 0004-0007 size = 48
// 0008-000B parent index = PREFAB_NO_PARENT
// 000C-0037 Serialised TransformComponent
// 0038-003B name_hash = "RenderableComponent"
// 003C-003F size = 9
// 0040-0048 Serialised RenderableComponent
// 0049-004C name_hash = "TransformComponent" (NEW ENTITY, index 1)
// 004D-0050 size = 48
// 0051-0054 parent index = 0
// 0055-0080 Serialised TransformComponent
// 0081-0084 name_hash = "LightComponent"
// 0085-0088 size = 21
// 0089-009D Serialised LightComponent

static_assert(std::numeric_limits<float>::is_iec559);
static_assert(sizeof(float) == 4);

constexpr uint32_t PREFAB_NO_PARENT = std::numeric_limits<uint32_t>::max();

// name_hash and size
constexpr size_t PREFAB_COMPONENT_HEADER_SIZE = 2 * sizeof(uint32_t);

} // namespace gcpak
