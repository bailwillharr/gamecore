#include "spin.h"

#include <tracy/Tracy.hpp>

#include <gamecore/gc_world.h>
#include <gamecore/gc_transform_component.h>

void SpinSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;
    m_world.forEach<gc::TransformComponent, SpinComponent>([&]([[maybe_unused]] gc::Entity entity, gc::TransformComponent& t, SpinComponent& s) {
        t.setRotation(glm::angleAxis(s.m_angle_radians, s.m_axis_norm));
        s.m_angle_radians += static_cast<float>(frame_state.delta_time) * s.m_radians_per_second;
    });
}