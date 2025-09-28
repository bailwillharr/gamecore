#include "mouse_move.h"

#include <tracy/Tracy.hpp>

#include <gamecore/gc_world.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>

void MouseMoveSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;
    m_world.forEach<gc::TransformComponent, MouseMoveComponent>([&]([[maybe_unused]] gc::Entity entity, gc::TransformComponent& t, MouseMoveComponent& m) {
        // change x and y positions by mouse delta
        t.setPosition(t.getPosition() + glm::vec3{frame_state.window_state->getMouseMotion() * m.sensitivity, 0.0f});
    });
}