#include "prefab_file.h"

#include <algorithm>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gamecore/gc_prefab.h>

namespace {

// parent index, then the transform
constexpr size_t ENTITY_DECLARATION_SIZE = sizeof(uint32_t) + gc::TransformComponent::getSerialisedSize();

// An entity that an earlier bake added: nothing but the baked component. It is replaced rather than added to.
bool isBakedEntity(const PrefabEntity& entity, gc::Name entity_name, gc::Name component_name)
{
    return entity.parent == gcpak::PREFAB_NO_PARENT && !entity.has_children && entity.transform.name == entity_name && entity.components.size() == 1 &&
           entity.components[0].name == component_name;
}

} // namespace

std::optional<std::vector<PrefabEntity>> readPrefab(std::span<const uint8_t> data)
{
    std::vector<PrefabEntity> entities{};
    gc::ByteReader reader(data);
    while (reader.remaining() > 0) {
        if (reader.remaining() < gcpak::PREFAB_COMPONENT_HEADER_SIZE) {
            return {};
        }
        const gc::Name component_name(reader.readU32());
        const size_t size = reader.readU32();
        if (size > reader.remaining()) {
            return {};
        }
        const std::span<const uint8_t> component_data = data.subspan(reader.pos(), size);
        reader.skip(size);

        if (component_name != gc::TransformComponent::NAME) {
            if (entities.empty()) {
                return {};
            }
            entities.back().components.push_back(PrefabComponent{component_name, component_data});
            continue;
        }

        // a new entity
        if (size != ENTITY_DECLARATION_SIZE) {
            return {};
        }
        PrefabEntity entity{};
        gc::ByteReader declaration_reader(component_data);
        entity.parent = declaration_reader.readU32();
        entity.transform.deserialise(declaration_reader);

        // the same matrix as the TransformSystem makes
        const glm::vec3 scale = entity.transform.getScale();
        glm::mat4 local_matrix = glm::mat4_cast(entity.transform.getRotation());
        local_matrix[0] *= scale.x;
        local_matrix[1] *= scale.y;
        local_matrix[2] *= scale.z;
        local_matrix[3] = glm::vec4(entity.transform.getPosition(), 1.0f);
        if (entity.parent == gcpak::PREFAB_NO_PARENT) {
            entity.matrix = local_matrix;
        }
        else {
            if (entity.parent >= entities.size()) {
                return {}; // parents are declared before their children
            }
            entities[entity.parent].has_children = true;
            entity.matrix = entities[entity.parent].matrix * local_matrix;
        }
        entities.push_back(std::move(entity));
    }
    if (entities.empty()) {
        return {};
    }
    return entities;
}

std::vector<uint8_t> writePrefabWithBakedComponent(const std::vector<PrefabEntity>& entities, gc::Name entity_name, gc::Name component_name,
                                                   std::span<const uint8_t> component_data)
{
    gc::PrefabWriter writer{};
    std::vector<uint32_t> new_indices(entities.size(), gcpak::PREFAB_NO_PARENT); // entities that are left out change the indices after them
    for (size_t i = 0; i < entities.size(); ++i) {
        const PrefabEntity& entity = entities[i];
        if (isBakedEntity(entity, entity_name, component_name)) {
            continue;
        }
        const uint32_t parent = (entity.parent == gcpak::PREFAB_NO_PARENT) ? gcpak::PREFAB_NO_PARENT : new_indices[entity.parent];
        new_indices[i] =
            writer.beginEntity(entity.transform.name, parent, entity.transform.getPosition(), entity.transform.getRotation(), entity.transform.getScale());
        for (const PrefabComponent& component : entity.components) {
            if (component.name == component_name) {
                continue; // out of date
            }
            const std::span<uint8_t> destination = writer.addComponentDeclaration(component.name, component.data.size());
            std::copy(component.data.begin(), component.data.end(), destination.begin());
        }
    }

    writer.beginEntity(entity_name);
    const std::span<uint8_t> destination = writer.addComponentDeclaration(component_name, component_data.size());
    std::copy(component_data.begin(), component_data.end(), destination.begin());

    const auto data = writer.getData();
    return std::vector<uint8_t>(data.begin(), data.end());
}
