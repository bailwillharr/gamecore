#include "gamecore/gc_render_system.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_app.h"
#include "gamecore/gc_resource_manager.h"

namespace gc {

RenderSystem::RenderSystem(gc::World& world, ResourceManager& resource_manager, RenderBackend& render_backend)
    : gc::System(world), m_render_object_manager(resource_manager, render_backend)
{
}

void RenderSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_world.forEach<TransformComponent, RenderableComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const RenderableComponent& c) {
        if (c.m_visible && !c.m_mesh.empty()) {
            // resolve resources
            RenderMesh* const mesh = m_render_object_manager.getRenderMesh(c.m_mesh);
            RenderMaterial* const material = m_render_object_manager.getRenderMaterial(c.m_material);
            if (mesh && material) {
                mesh->setLastUsedFrame(frame_state.frame_count);
                material->setLastUsedFrame(frame_state.frame_count);
                frame_state.draw_data.drawMesh(t.getWorldMatrix(), mesh, material);
            }
        }
    });

    constexpr uint64_t INACTIVE_OBJECT_LIFETIME_FRAMES = 10;
    if (frame_state.frame_count > INACTIVE_OBJECT_LIFETIME_FRAMES) {
        m_render_object_manager.deleteUnusedObjects(frame_state.frame_count - INACTIVE_OBJECT_LIFETIME_FRAMES);
    }
}

} // namespace gc
