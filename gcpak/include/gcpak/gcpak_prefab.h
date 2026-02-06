#pragma once

#include <cstdint>

#include <array>
#include <limits>
#include <iostream>

namespace gcpak {

// An instantiatable entity tree.
// Designed to be efficiently loaded into the world.
// Contains a packed list component declarations. (component declarations are not neccesarily the same size)
// Order of entities in the list must match hierarchy order (no children before parent)
// Root entity must be index zero.
// A new entity is declared with a TransformComponent declaration. TransformComponent == ENTITY BEGIN MARKER
// No other component type can appear before the first TransformComponent
// Any references to other entity IDs in component declarations are the
// index of the referenced entity in order of declaration in the prefab.
// First field in all component declarations is PrefabComponentType.

static_assert(std::numeric_limits<float>::is_iec559);
static_assert(sizeof(float) == 4);

enum class PrefabComponentType : uint32_t {
    TRANSFORM = 0,
};

struct PrefabComponentTransform {
    // new entity declaration
    PrefabComponentType type;
    uint32_t name_crc32;
    uint32_t parent_entity_index; // that entity's index in this list
    std::array<float, 3> pos_xyz;
    std::array<float, 4> rot_wxyz;
    std::array<float, 3> scale_xyz;

    void serialize(std::ostream& s) const
    {
        s.write(reinterpret_cast<const char*>(&type), sizeof(type));
        s.write(reinterpret_cast<const char*>(&name_crc32), sizeof(name_crc32));
        s.write(reinterpret_cast<const char*>(&parent_entity_index), sizeof(parent_entity_index));
        s.write(reinterpret_cast<const char*>(&pos_xyz), sizeof(pos_xyz));
        s.write(reinterpret_cast<const char*>(&rot_wxyz), sizeof(rot_wxyz));
        s.write(reinterpret_cast<const char*>(&scale_xyz), sizeof(scale_xyz));
    }

    static PrefabComponentTransform deserialize(std::istream& s)
    {
        PrefabComponentTransform t{};
        s.read(reinterpret_cast<char*>(&t.type), sizeof(t.type));
        s.read(reinterpret_cast<char*>(&t.name_crc32), sizeof(t.name_crc32));
        s.read(reinterpret_cast<char*>(&t.parent_entity_index), sizeof(t.parent_entity_index));
        s.read(reinterpret_cast<char*>(&t.pos_xyz), sizeof(t.pos_xyz));
        s.read(reinterpret_cast<char*>(&t.rot_wxyz), sizeof(t.rot_wxyz));
        s.read(reinterpret_cast<char*>(&t.scale_xyz), sizeof(t.scale_xyz));
        return t;
    }

    static consteval size_t getSerializedSize()
    {
        return sizeof(type) + sizeof(name_crc32) + sizeof(parent_entity_index) + sizeof(pos_xyz) + sizeof(rot_wxyz) + sizeof(scale_xyz);
    }
};

static_assert(sizeof(PrefabComponentTransform) == 52);
static_assert(PrefabComponentTransform::getSerializedSize() == 52);

} // namespace gcpak