#include "gamecore/gc_render_system.h"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_fog_component.h"
#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_resources.h"
#include "gamecore/gc_shadow_map_component.h"
#include "gamecore/gc_light_component.h"
#include "gamecore/gc_skybox_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_app.h"
#include "gamecore/gc_resource_manager.h"

namespace gc {

RenderSystem::RenderSystem(gc::World& world, ResourceManager& resource_manager, RenderBackend& render_backend)
    : gc::System(world), m_resource_manager(resource_manager), m_render_backend(render_backend), m_render_object_manager(resource_manager, render_backend)
{
}

void RenderSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    constexpr uint64_t INACTIVE_OBJECT_LIFETIME_FRAMES = 10;
    constexpr int AUTOMATIC_INSTANCING_THRESHOLD = 8;

    m_instance_groups.clear();

    m_world.forEach<TransformComponent, RenderableComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const RenderableComponent& c) {
        if (c.m_visible && !c.m_mesh.empty()) [[likely]] {
            // resolve resources
            RenderMesh* const mesh = m_render_object_manager.getRenderMesh(c.m_mesh);
            RenderMaterial* const material = m_render_object_manager.getRenderMaterial(c.m_material);
            if (mesh && material) [[likely]] {
                m_instance_groups[{mesh, material}].push_back(t.getWorldMatrix());
            }
        }
    });

    for (const auto& [mesh_material, transforms] : m_instance_groups) {

        RenderMesh* const mesh = mesh_material.first;
        RenderMaterial* const material = mesh_material.second;

        mesh->setLastUsedFrame(frame_state.frame_count);
        material->setLastUsedFrame(frame_state.frame_count);

        GC_ASSERT(transforms.size() != 0);
        // (blended materials are drawn one at a time, as they have to be sorted by distance)
        if (transforms.size() < AUTOMATIC_INSTANCING_THRESHOLD || material->getBlendMode() == MaterialBlendMode::ALPHA_BLEND) {
            for (const auto& transform : transforms) {
                frame_state.draw_data.drawMesh(transform, mesh, material);
            }
        }
        else {
            frame_state.draw_data.drawMeshInstanced(transforms, mesh, material);
        }
    }

    // The directional light's baked shadows. A world that doesn't use them doesn't have to register the component.
    if (m_world.isComponentRegistered(ShadowMapComponent::NAME)) {
        m_world.forEach<TransformComponent, ShadowMapComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const ShadowMapComponent& c) {
            if (c.m_shadow_map.empty()) {
                return;
            }
            if (c.m_shadow_map != m_shadow_map_name) {
                m_shadow_map_name = c.m_shadow_map;
                m_shadow_map.reset();
                if (const ResourceShadowMap* const resource = m_resource_manager.get<ResourceShadowMap>(c.m_shadow_map); resource) {
                    m_shadow_map = std::make_unique<RenderShadowMap>(
                        m_render_backend.createShadowMap(resource->data.get(), getRenderUploadMode(m_resource_manager, resource->data.get())));
                }
                else {
                    GC_ERROR("Shadow map not found: {}", c.m_shadow_map);
                }
            }
            if (m_shadow_map) {
                // the component's matrix is relative to its entity, so that the shadows move with it
                frame_state.draw_data.setShadowMap(m_shadow_map.get(), c.m_matrix * glm::inverse(t.getWorldMatrix()), c.m_normal_bias, c.m_depth_bias);
            }
        });
    }

    // The sky. A world that doesn't use one doesn't have to register the component.
    if (m_world.isComponentRegistered(SkyboxComponent::NAME)) {
        m_world.forEach<TransformComponent, SkyboxComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const SkyboxComponent& c) {
            if (c.m_sky.empty() || !(c.m_illuminance > 0.0f)) {
                return;
            }
            const std::array<Name, 3> names{c.m_sky, c.m_irradiance, c.m_specular};
            if (names != m_environment_names) {
                m_environment_names = names;
                m_environment.reset();
                const ResourceCubeMap* const sky = m_resource_manager.get<ResourceCubeMap>(c.m_sky);
                const ResourceCubeMap* const irradiance = m_resource_manager.get<ResourceCubeMap>(c.m_irradiance);
                const ResourceCubeMap* const specular = m_resource_manager.get<ResourceCubeMap>(c.m_specular);
                if (sky && irradiance && specular) {
                    // (all three are in the content, or none of them are)
                    const bool in_content = getRenderUploadMode(m_resource_manager, sky->data.get()) == RenderUploadMode::BACKGROUND &&
                                            getRenderUploadMode(m_resource_manager, irradiance->data.get()) == RenderUploadMode::BACKGROUND &&
                                            getRenderUploadMode(m_resource_manager, specular->data.get()) == RenderUploadMode::BACKGROUND;
                    m_environment = std::make_unique<RenderEnvironment>(
                        m_render_backend.createEnvironment(sky->data.get(), irradiance->data.get(), specular->data.get(),
                                                           in_content ? RenderUploadMode::BACKGROUND : RenderUploadMode::IMMEDIATE));
                    m_environment_info = sky->info;
                }
                else {
                    GC_ERROR("The cube maps of a sky weren't all found: {}, {}, {}", c.m_sky, c.m_irradiance, c.m_specular);
                }
            }
            if (!m_environment) {
                return;
            }

            // The sky is infinitely far away, so only the way its entity is turned matters
            glm::mat3 sky_to_world{t.getWorldMatrix()};
            for (int axis = 0; axis < 3; ++axis) {
                const float length = glm::length(sky_to_world[axis]);
                sky_to_world[axis] = (length > 0.0f) ? sky_to_world[axis] / length : glm::mat3{1.0f}[axis];
            }

            if (c.m_follow_sun && m_environment_info.sun_illuminance > 0.0f && m_world.isComponentRegistered(LightComponent::NAME)) {
                // Turn the sky about the vertical until its sun is over the world's directional light (the last one, as that is
                // the one that is drawn)
                glm::vec3 to_light{0.0f};
                m_world.forEach<TransformComponent, LightComponent>([&](Entity, const TransformComponent& light_transform, const LightComponent& light) {
                    if (light.m_type == LightType::DIRECTIONAL) {
                        to_light = glm::vec3(light_transform.getWorldMatrix()[2]);
                    }
                });
                const glm::vec3 to_sun = sky_to_world * m_environment_info.sun_direction;
                constexpr float MIN_HORIZONTAL = 1.0e-4f; // something straight overhead has no compass direction
                if (std::hypot(to_light.x, to_light.y) > MIN_HORIZONTAL && std::hypot(to_sun.x, to_sun.y) > MIN_HORIZONTAL) {
                    const float angle = std::atan2(to_light.y, to_light.x) - std::atan2(to_sun.y, to_sun.x);
                    sky_to_world = glm::mat3(glm::rotate(glm::mat4{1.0f}, angle, glm::vec3{0.0f, 0.0f, 1.0f})) * sky_to_world;
                }
            }

            // The asset only says how bright each part of the sky is compared to the rest. The component says how bright it all is
            const float scale = (m_environment_info.sky_illuminance > 0.0f) ? c.m_illuminance / m_environment_info.sky_illuminance : 1.0f;
            frame_state.draw_data.setEnvironment(m_environment.get(), glm::mat4(glm::transpose(sky_to_world)), scale, c.m_visible, c.m_lighting);
        });
    }

    // Haze. A world that doesn't use it doesn't have to register the component.
    if (m_world.isComponentRegistered(FogComponent::NAME)) {
        m_world.forEach<TransformComponent, FogComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const FogComponent& c) {
            WorldFog fog{};
            fog.density = c.m_density;
            fog.height_falloff = c.m_height_falloff;
            fog.base_height = t.getWorldMatrix()[3].z;
            fog.color = c.m_color;
            fog.use_sky = c.m_use_sky;
            frame_state.draw_data.setFog(fog);
        });
    }

    if (frame_state.frame_count > INACTIVE_OBJECT_LIFETIME_FRAMES) {
        m_render_object_manager.deleteUnusedObjects(frame_state.frame_count - INACTIVE_OBJECT_LIFETIME_FRAMES);
    }
}

} // namespace gc
