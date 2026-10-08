#pragma once

// Reads a prefab's data without loading it into a World, and writes it back with something baked into it.
// This is what the bakers (prefab_shadow_baker.h, prefab_collision_baker.h) use to look at a prefab and to add their results.

#include <cstdint>

#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>

#include <gcpak/gcpak_prefab.h>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_transform_component.h>

struct PrefabComponent {
    gc::Name name{};
    std::span<const uint8_t> data{}; // part of the data that the prefab was read from
};

struct PrefabEntity {
    uint32_t parent{gcpak::PREFAB_NO_PARENT};
    gc::TransformComponent transform{};
    std::vector<PrefabComponent> components{};
    glm::mat4 matrix{1.0f}; // from the entity's space to prefab space
    bool has_children{false};
};

// See gcpak_prefab.h for the format. Nothing in the data is trusted. Empty if it is corrupt.
// The entities refer to the data, so it has to stay where it is while they are used.
std::optional<std::vector<PrefabEntity>> readPrefab(std::span<const uint8_t> data);

// Reads a serialised component, if it is the size that this version of the component expects
template <typename T>
std::optional<T> readComponent(const PrefabComponent& component)
{
    if (component.name != T::NAME || component.data.size() != T::getSerialisedSize()) {
        return {};
    }
    T value{};
    gc::ByteReader reader(component.data);
    value.deserialise(reader);
    return value;
}

// The prefab again, with a new root entity that has nothing but the baked component. The root has no transform of its own, so
// the component is in prefab space. What an earlier bake added is left out: every component with that name, and the entity
// with that name that was made for it.
std::vector<uint8_t> writePrefabWithBakedComponent(const std::vector<PrefabEntity>& entities, gc::Name entity_name, gc::Name component_name,
                                                   std::span<const uint8_t> component_data);

template <typename T>
std::vector<uint8_t> writePrefabWithBakedComponent(const std::vector<PrefabEntity>& entities, gc::Name entity_name, const T& component)
{
    std::vector<uint8_t> component_data(T::getSerialisedSize());
    gc::ByteWriter writer(component_data);
    component.serialise(writer);
    return writePrefabWithBakedComponent(entities, entity_name, T::NAME, component_data);
}
