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
    float move_up_vector{};
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
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_SPACE)) {
        move_up_vector += 1.0f;
    }
    if (frame_state.window_state->getKeyDown(SDL_SCANCODE_LSHIFT)) {
        move_up_vector -= 1.0f;
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

        const glm::quat pitch = glm::angleAxis(mr.m_pitch, glm::vec3{1.0f, 0.0f, 0.0f});
        const glm::quat yaw = glm::angleAxis(-mr.m_yaw, glm::vec3{0.0f, 0.0f, 1.0f});
        const glm::quat rotation = yaw * pitch;

        // Don't use pitch for these vectors as W and S should go forward and back not up and down too
        const glm::vec3 forward = yaw * glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::vec3 right = yaw * glm::vec3{1.0f, 0.0f, 0.0f};
        glm::vec3 move_direction = move_forward_vector * forward + move_right_vector * right;
        move_direction.z = move_up_vector;
        if (glm::dot(move_direction, move_direction) > 0.0f) {
            mr.m_current_velocity += glm::normalize(move_direction) * mr.m_acceleration * static_cast<float>(frame_state.delta_time);
            const float length2 = glm::dot(mr.m_current_velocity, mr.m_current_velocity);
            if (length2 > mr.m_move_speed * mr.m_move_speed) {
                // clamp velocity magnitude to m_move_speed
                mr.m_current_velocity = glm::normalize(mr.m_current_velocity) * mr.m_move_speed;
            }
        }
        else if (glm::dot(mr.m_current_velocity, mr.m_current_velocity) != 0.0f) { // no input and velocity isn't zero
            // no input. decelerate
            glm::vec3 prev_velocity = mr.m_current_velocity;
            mr.m_current_velocity -= glm::normalize(mr.m_current_velocity) * mr.m_deceleration * static_cast<float>(frame_state.delta_time);
            if (glm::dot(prev_velocity, mr.m_current_velocity) < 0.0f) { // if signs are different
                mr.m_current_velocity = {0.0f, 0.0f, 0.0f};
            }
        }

        glm::vec3 position = t.getPosition();
        position += mr.m_current_velocity * static_cast<float>(frame_state.delta_time);
        frame_state.current_velocity = mr.m_current_velocity;
        t.setRotation(rotation);
        t.setPosition(position);
    });
}