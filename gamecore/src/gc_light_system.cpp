#include "gamecore/gc_light_system.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_world.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_light_component.h"
#include "gamecore/gc_frame_state.h"

namespace gc {

LightSystem::LightSystem(gc::World& world) : gc::System(world) {}

void LightSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_world.forEach<TransformComponent, LightComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, [[maybe_unused]] const LightComponent& l) {
        frame_state.draw_data.setLightPos(t.getWorldPosition());
        // TODO: support more than one light!
    });
}

} // namespace gc
