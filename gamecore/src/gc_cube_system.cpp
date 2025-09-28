#include "gamecore/gc_cube_system.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_cube_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_frame_state.h"

namespace gc {

CubeSystem::CubeSystem(gc::World& world) : gc::System(world) {}

void CubeSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_world.forEach<TransformComponent, CubeComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t, CubeComponent& c) {
        if (c.visible && c.mesh) {
            frame_state.draw_data.drawMesh(t.getWorldMatrix(), c.mesh);
        }
    });
}

} // namespace gc
