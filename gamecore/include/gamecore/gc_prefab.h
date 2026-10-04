#pragma once

// Prefabs are trees of entities and their components, stored as PREFAB assets in .gcpak files. See gcpak_prefab.h for the format.
// A prefab can be anything from a single prop to an entire game world.
//
// Any component that is a SerialisableComponent (see gc_ecs.h) can be stored in a prefab, including components defined by the game.
// Components are identified by their NAME, so a prefab can only be loaded fully into a World that has registered the components
// it uses. Components that the World doesn't know about are skipped.
//
// Prefabs don't contain resources. A component that uses a resource just stores its Name, as RenderableComponent does.

#include <cstdint>

#include <span>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <gcpak/gcpak_prefab.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

namespace gc {

class World;   // forward-dec
class Content; // forward-dec

// Creates the prefab's entities in the world. The roots of the prefab become children of prefab_parent.
// Returns the prefab's first root entity, or ENTITY_NONE on failure, in which case nothing is added to the world.
// If entities_out is given, every entity that was created is appended to it, in the order they appear in the prefab.
Entity loadPrefab(std::span<const uint8_t> data, World& world, Entity prefab_parent = ENTITY_NONE, std::vector<Entity>* entities_out = nullptr);

// Same, with the prefab found by its asset ID.
Entity loadPrefab(const Content& content, Name prefab_name, World& world, Entity prefab_parent = ENTITY_NONE,
                  std::vector<Entity>* entities_out = nullptr);

// Makes a prefab from an entity and all of its descendants. The result can be added to a .gcpak file as a PREFAB asset.
// Components that aren't serialisable are left out.
std::vector<uint8_t> savePrefab(World& world, Entity root);

// Makes a prefab from every entity in the world.
std::vector<uint8_t> saveWorldAsPrefab(World& world);

// Builds a prefab without needing a World.
class PrefabWriter {
    std::vector<uint8_t> m_data{};
    uint32_t m_entity_count{};

public:
    // Starts a new entity. Components added from now on belong to it. Returns its index, for use as the parent of later entities.
    // parent_index must be an entity that was already added, or gcpak::PREFAB_NO_PARENT for a root.
    uint32_t beginEntity(Name name, uint32_t parent_index = gcpak::PREFAB_NO_PARENT, const glm::vec3& position = glm::vec3{0.0f, 0.0f, 0.0f},
                         const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, const glm::vec3& scale = glm::vec3{1.0f, 1.0f, 1.0f});

    // Adds a component to the current entity. Don't add the TransformComponent, beginEntity() does that.
    template <SerialisableComponent T>
    void addComponent(const T& component)
    {
        ByteWriter writer(addComponentDeclaration(T::NAME, T::getSerialisedSize()));
        component.serialise(writer);
        GC_ASSERT(writer.remaining() == 0);
    }

    // Adds an empty declaration for a component of the current entity and returns where its serialised data should be written.
    // The returned span is only valid until something else is added.
    std::span<uint8_t> addComponentDeclaration(Name component_name, size_t size);

    uint32_t getEntityCount() const;

    std::span<const uint8_t> getData() const;
};

} // namespace gc
