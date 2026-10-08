#include "prefab_shadow_baker.h"

#include <algorithm>
#include <format>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_light_component.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_stopwatch.h>

#include "prefab_file.h"

namespace {

constexpr gc::Name SHADOW_MAP_ENTITY_NAME = gc::Name::createConstexpr("shadow_map");

} // namespace

PrefabShadowResult bakePrefabShadows(const std::string& prefab_name, std::vector<uint8_t>& prefab_data, const AssetLookup& find_asset,
                                     const PrefabShadowOptions& options, Asset& shadow_map_out)
{
    const std::optional<std::vector<PrefabEntity>> entities = readPrefab(prefab_data);
    if (!entities) {
        GC_ERROR("{}: the prefab is corrupt", prefab_name);
        return PrefabShadowResult::CORRUPT;
    }

    gc::Stopwatch stopwatch{};

    std::optional<glm::vec3> direction_to_light{};
    std::vector<ShadowCaster> casters{};
    uint32_t missing_meshes = 0;
    std::vector<bool> excluded(entities->size(), false); // from casting shadows
    for (size_t i = 0; i < entities->size(); ++i) {
        const PrefabEntity& entity = (*entities)[i];
        // children come after their parents, so a parent's exclusion is already known
        const bool is_excluded_by_name =
            std::find(options.excluded_entities.begin(), options.excluded_entities.end(), entity.transform.name) != options.excluded_entities.end();
        excluded[i] = is_excluded_by_name || (entity.parent != gcpak::PREFAB_NO_PARENT && excluded[entity.parent]);

        for (const PrefabComponent& component : entity.components) {
            if (const auto light = readComponent<gc::LightComponent>(component); light && light->m_type == gc::LightType::DIRECTIONAL) {
                // The light shines along its -Z axis, so its +Z axis points towards it.
                const glm::vec3 z_axis{entity.matrix[2]};
                if (glm::dot(z_axis, z_axis) > 0.0f) {
                    direction_to_light = glm::normalize(z_axis);
                }
            }

            const auto renderable = readComponent<gc::RenderableComponent>(component);
            if (!renderable || excluded[i] || !renderable->m_visible || renderable->m_mesh.empty()) {
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

    // Only one shadow map is drawn per world, so the one that the prefab had (if it was baked before) is replaced
    prefab_data = writePrefabWithBakedComponent(
        *entities, SHADOW_MAP_ENTITY_NAME,
        gc::ShadowMapComponent{}.setShadowMap(gc::Name(asset_name)).setMatrix(shadow_map->matrix).setBias(normal_bias, depth_bias));

    GC_INFO("{}: baked a {}x{} shadow map of {} triangles ({} meshes) in {}. Its texels are {:.3f} m wide", prefab_name, shadow_map->resolution,
            shadow_map->resolution, shadow_map->triangle_count, casters.size(), stopwatch, shadow_map->texel_size);
    if (missing_meshes > 0) {
        GC_WARN("{}: {} meshes weren't found and cast no shadows", prefab_name, missing_meshes);
    }
    return PrefabShadowResult::BAKED;
}
