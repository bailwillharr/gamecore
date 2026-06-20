#include "gamecore/gc_prefab.h"

#include <vector>

#include <gctemplates/gct_sv_stream.h>

#include <gcpak/gcpak_prefab.h>

#include "gclog/gclog.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_world.h"

namespace gc {

static Entity deserialiseTransform(gct::sv_istream& data_stream, World& world, std::span<const Entity> entities, Entity prefab_parent)
{
    uint32_t parent_index{};
    auto deserialised_transform = TransformComponent::deserialize(data_stream, parent_index);
    if (!data_stream) {
        return ENTITY_NONE;
    }

    // figure out parent entity
    Entity parent{prefab_parent};
    if (parent_index != ENTITY_NONE && parent_index < static_cast<uint32_t>(entities.size())) {
        parent = entities[parent_index];
    }

    return world.createEntity(deserialised_transform.name, parent, deserialised_transform.getPosition(), deserialised_transform.getRotation(),
                              deserialised_transform.getScale());
}

Entity loadPrefab(std::span<const uint8_t> data, World& world, Entity prefab_parent)
{
    const std::string_view data_sv(reinterpret_cast<const char*>(data.data()), data.size());
    gct::sv_istream data_stream(data_sv);

    std::vector<Entity> entities{};

    while (!data_stream.eof()) {

        gcpak::PrefabComponentType type{};
        data_stream.read(reinterpret_cast<char*>(&type), sizeof(gcpak::PrefabComponentType));
        if (!data_stream) {
            abortGame("Failed to read prefab component type");
        }

        if (entities.empty() && type != gcpak::PrefabComponentType::TRANSFORM) {
            abortGame("Corrupt prefab asset");
        }

        Entity current_entity{ENTITY_NONE};
        if (!entities.empty()) {
            current_entity = entities.back();
        }

        switch (type) {
        case gcpak::PrefabComponentType::TRANSFORM: {
            Entity e = deserialiseTransform(data_stream, world, entities, prefab_parent);
            if (e == ENTITY_NONE) {
                abortGame("Error reading transform from prefab");
            }
            entities.push_back(e);
        } break;
        case gcpak::PrefabComponentType::RENDERABLE: {
            const auto r = RenderableComponent::deserialize(data_stream);
            if (!data_stream) {
                abortGame("Error deserialising RenderableComponent from prefab");
            }
            if (world.getComponent<RenderableComponent>(current_entity)) {
                abortGame("Duplicate component in prefab entity");
            }
            world.addComponent<RenderableComponent>(current_entity) = r;
        } break;
        case gcpak::PrefabComponentType::CAMERA:
            [[fallthrough]];
        case gcpak::PrefabComponentType::LIGHT:
            [[fallthrough]];
        default:
            abortGame("Unsupported component type {} found in prefab", static_cast<std::underlying_type_t<gcpak::PrefabComponentType>>(type));
        }
    }

    return entities[0];
}

} // namespace gc