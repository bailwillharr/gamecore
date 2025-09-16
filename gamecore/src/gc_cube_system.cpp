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

    frame_state.cube_transforms.clear();

    m_world.forEach<TransformComponent, CubeComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t, CubeComponent& c) {
        if (c.visible) {
            frame_state.cube_transforms.push_back(t.getWorldMatrix());
        }
    });
}

} // namespace gc