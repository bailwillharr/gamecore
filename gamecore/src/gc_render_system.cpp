#include "gamecore/gc_render_system.h"

#include <glm/matrix.hpp>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_resources.h"
#include "gamecore/gc_shadow_map_component.h"
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
                    m_shadow_map = std::make_unique<RenderShadowMap>(m_render_backend.createShadowMap(resource->data.get()));
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

    if (frame_state.frame_count > INACTIVE_OBJECT_LIFETIME_FRAMES) {
        m_render_object_manager.deleteUnusedObjects(frame_state.frame_count - INACTIVE_OBJECT_LIFETIME_FRAMES);
    }
}

} // namespace gc
