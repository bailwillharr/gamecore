#include "prefab_shadow_baker.h"

#include <algorithm>
#include <format>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gcpak/gcpak_prefab.h>

#include <gclog/gclog.h>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_stopwatch.h>
#include <gamecore/gc_transform_component.h>

namespace {

constexpr gc::Name SHADOW_MAP_ENTITY_NAME = gc::Name::createConstexpr("shadow_map");

// parent index, then the transform
constexpr size_t ENTITY_DECLARATION_SIZE = sizeof(uint32_t) + gc::TransformComponent::getSerialisedSize();

struct PrefabComponent {
    gc::Name name{};
    std::span<const uint8_t> data{};
};

struct PrefabEntity {
    uint32_t parent{gcpak::PREFAB_NO_PARENT};
    gc::TransformComponent transform{};
    std::vector<PrefabComponent> components{};
    glm::mat4 matrix{1.0f}; // from the entity's space to prefab space
    bool has_children{false};
    bool excluded{false}; // from casting shadows
};

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

// See gcpak_prefab.h for the format. Nothing in the data is trusted. Empty if it is corrupt.
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

// An entity that an earlier bake added: nothing but a ShadowMapComponent. It is replaced rather than added to.
bool isBakedShadowMapEntity(const PrefabEntity& entity)
{
    return entity.parent == gcpak::PREFAB_NO_PARENT && !entity.has_children && entity.transform.name == SHADOW_MAP_ENTITY_NAME &&
           entity.components.size() == 1 && entity.components[0].name == gc::ShadowMapComponent::NAME;
}

// The prefab again, without the shadow maps it had, and with the new one
std::vector<uint8_t> writePrefab(const std::vector<PrefabEntity>& entities, const gc::ShadowMapComponent& shadow_map)
{
    gc::PrefabWriter writer{};
    std::vector<uint32_t> new_indices(entities.size(), gcpak::PREFAB_NO_PARENT); // entities that are left out change the indices after them
    for (size_t i = 0; i < entities.size(); ++i) {
        const PrefabEntity& entity = entities[i];
        if (isBakedShadowMapEntity(entity)) {
            continue;
        }
        const uint32_t parent = (entity.parent == gcpak::PREFAB_NO_PARENT) ? gcpak::PREFAB_NO_PARENT : new_indices[entity.parent];
        new_indices[i] =
            writer.beginEntity(entity.transform.name, parent, entity.transform.getPosition(), entity.transform.getRotation(), entity.transform.getScale());
        for (const PrefabComponent& component : entity.components) {
            if (component.name == gc::ShadowMapComponent::NAME) {
                continue; // only one shadow map is drawn per world, and this one is out of date
            }
            const std::span<uint8_t> destination = writer.addComponentDeclaration(component.name, component.data.size());
            std::copy(component.data.begin(), component.data.end(), destination.begin());
        }
    }

    // A root with no transform of its own, so the shadow map's matrix is from prefab space
    writer.beginEntity(SHADOW_MAP_ENTITY_NAME);
    writer.addComponent(shadow_map);

    const auto data = writer.getData();
    return std::vector<uint8_t>(data.begin(), data.end());
}

} // namespace

PrefabShadowResult bakePrefabShadows(const std::string& prefab_name, std::vector<uint8_t>& prefab_data, const AssetLookup& find_asset,
                                     const PrefabShadowOptions& options, Asset& shadow_map_out)
{
    std::optional<std::vector<PrefabEntity>> entities = readPrefab(prefab_data);
    if (!entities) {
        GC_ERROR("{}: the prefab is corrupt", prefab_name);
        return PrefabShadowResult::CORRUPT;
    }

    gc::Stopwatch stopwatch{};

    std::optional<glm::vec3> direction_to_light{};
    std::vector<ShadowCaster> casters{};
    uint32_t missing_meshes = 0;
    for (PrefabEntity& entity : *entities) {
        // children come after their parents, so a parent's exclusion is already known
        const bool is_excluded_by_name =
            std::find(options.excluded_entities.begin(), options.excluded_entities.end(), entity.transform.name) != options.excluded_entities.end();
        entity.excluded = is_excluded_by_name || (entity.parent != gcpak::PREFAB_NO_PARENT && (*entities)[entity.parent].excluded);

        for (const PrefabComponent& component : entity.components) {
            if (const auto light = readComponent<gc::LightComponent>(component); light && light->m_type == gc::LightType::DIRECTIONAL) {
                // The light shines along its -Z axis, so its +Z axis points towards it.
                const glm::vec3 z_axis{entity.matrix[2]};
                if (glm::dot(z_axis, z_axis) > 0.0f) {
                    direction_to_light = glm::normalize(z_axis);
                }
            }

            const auto renderable = readComponent<gc::RenderableComponent>(component);
            if (!renderable || entity.excluded || !renderable->m_visible || renderable->m_mesh.empty()) {
                continue;
            }
            const Asset* const mesh = find_asset(renderable->m_mesh);
            if (!mesh || mesh->type != gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16) {
                // e.g. a mesh that the game generates when it starts
                GC_WARN("{}: the mesh {} of entity '{}' wasn't found, so it casts no shadow", prefab_name, renderable->m_mesh, entity.transform.name);
                ++missing_meshes;
                continue;
            }
            ShadowCaster caster{};
            caster.mesh_data = mesh->data;
            caster.matrix = entity.matrix;

            // The holes of alpha tested materials don't cast shadows. Alpha blended materials can't cast partial shadows, as a
            // shadow map only has one depth, so they cast a full shadow where they are mostly opaque and none elsewhere.
            // A material that can't be found (or the default material) is taken to be opaque.
            const Asset* const material_asset = renderable->m_material.empty() ? nullptr : find_asset(renderable->m_material);
            if (material_asset && material_asset->type == gcpak::GcpakAssetType::MATERIAL) {
                const std::optional<gc::ResourceMaterial> material = gc::ResourceMaterial::create(material_asset->data);
                if (material && material->blend_mode != gc::MaterialBlendMode::NONE) {
                    caster.alpha_cutoff = (material->blend_mode == gc::MaterialBlendMode::ALPHA_TEST) ? material->alpha_cutoff : 0.5f;
                    caster.alpha = material->base_color.a;
                    if (const Asset* const texture = find_asset(material->base_color_texture); texture && isTextureType(texture->type)) {
                        caster.alpha_texture_data = texture->data;
                    }
                }
            }
            casters.push_back(caster);
        }
    }

    if (!direction_to_light) {
        GC_INFO("{}: has no directional light, so there are no shadows to bake", prefab_name);
        return PrefabShadowResult::NO_LIGHT;
    }
    if (casters.empty()) {
        GC_WARN("{}: nothing in the prefab casts a shadow", prefab_name);
        return PrefabShadowResult::NO_CASTERS;
    }

    const std::optional<BakedShadowMap> shadow_map = bakeShadowMap(casters, *direction_to_light, options.resolution);
    if (!shadow_map) {
        GC_WARN("{}: nothing in the prefab casts a shadow", prefab_name);
        return PrefabShadowResult::NO_CASTERS;
    }

    const std::string asset_name = std::format("{}/shadowmap", prefab_name);
    shadow_map_out = makeAsset(asset_name, makeShadowMapData(*shadow_map), gcpak::GcpakAssetType::SHADOW_MAP_R16);

    // Surfaces are moved off themselves by a couple of texels before they are looked up, and a little towards the light: by
    // the depth that a texel covers on a surface at 45 degrees to the light, plus what is lost by storing depths in 16 bits.
    const float normal_bias = 2.0f * shadow_map->texel_size;
    const float depth_bias = shadow_map->texel_size / shadow_map->depth_range + 4.0f / 65535.0f;

    prefab_data =
        writePrefab(*entities, gc::ShadowMapComponent{}.setShadowMap(gc::Name(asset_name)).setMatrix(shadow_map->matrix).setBias(normal_bias, depth_bias));

    GC_INFO("{}: baked a {}x{} shadow map of {} triangles ({} meshes) in {}. Its texels are {:.3f} m wide", prefab_name, shadow_map->resolution,
            shadow_map->resolution, shadow_map->triangle_count, casters.size(), stopwatch, shadow_map->texel_size);
    if (missing_meshes > 0) {
        GC_WARN("{}: {} meshes weren't found and cast no shadows", prefab_name, missing_meshes);
    }
    return PrefabShadowResult::BAKED;
}
