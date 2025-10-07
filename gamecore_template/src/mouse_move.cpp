#include "mouse_move.h"

#include <tracy/Tracy.hpp>

#include <glm.hpp>

#include <gamecore/gc_world.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>

void MouseMoveSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;
    const auto& mouse_motion = frame_state.window_state->getMouseMotion();
    float move_forward_vector{};
    float move_right_vector{};
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_W)) {
        move_forward_vector += 1.0f;
    }
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_S)) {
        move_forward_vector -= 1.0f;
    }
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_D)) {
        move_right_vector += 1.0f;
    }
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_A)) {
        move_right_vector -= 1.0f;
    }
    m_world.forEach<gc::TransformComponent, MouseMoveComponent>([&]([[maybe_unused]] gc::Entity entity, gc::TransformComponent& t, MouseMoveComponent& mr) {
        // Camera point towards -Z in local space
        mr.m_yaw += mouse_motion.x * mr.m_sensitivity;
        mr.m_pitch += mouse_motion.y * mr.m_sensitivity;
        if (mr.m_pitch > glm::pi<float>()) {
            mr.m_pitch = glm::pi<float>();
        }
        else if (mr.m_pitch < 0.0f) {
            mr.m_pitch = 0.0f;
        }

        const glm::quat rotation = glm::angleAxis(-mr.m_yaw, glm::vec3{0.0f, 0.0f, 1.0f}) * glm::angleAxis(mr.m_pitch, glm::vec3{1.0f, 0.0f, 0.0f});

        glm::vec3 position = t.getPosition();
        glm::vec3 forward = rotation * glm::vec3{0.0f, 0.0f, -1.0f};
        glm::vec3 right = rotation * glm::vec3{1.0f, 0.0f, 0.0f};
        glm::vec3 move_direction = move_forward_vector * forward + move_right_vector * right;
        if (glm::dot(move_direction, move_direction) > 0.0f) {
            position += glm::normalize(move_direction) * static_cast<float>(frame_state.delta_time) * mr.m_move_speed;
        }

        t.setRotation(rotation);
        t.setPosition(position);
    });
}