#include "gamecore/gc_render_system.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_app.h"
#include "gamecore/gc_resource_manager.h"

namespace gc {

RenderSystem::RenderSystem(gc::World& world) : gc::System(world) {}

void RenderSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_world.forEach<TransformComponent, RenderableComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t, RenderableComponent& c) {
        if (c.m_visible && !c.m_mesh.empty() && !c.m_material.empty()) {
            // resolve resources
            ResourceManager& rm = app().resourceManager();
            auto& mesh = rm.get<RenderMesh>(c.m_mesh);
            auto& material = rm.get<RenderMaterial>(c.m_material);

            frame_state.draw_data.drawMesh(t.getWorldMatrix(), c.m_mesh, c.m_material);
        }
        if (t.name == Name("light")) {
            frame_state.draw_data.setLightPos(t.getWorldPosition());
        }
    });
}

} // namespace gc
