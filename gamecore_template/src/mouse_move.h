#pragma once

#include <glm/vec3.hpp>
#include <glm/gtc/constants.hpp>

#include <gamecore/gc_ecs.h>

class MouseMoveSystem; // forward-dec

class MouseMoveComponent {
    friend class MouseMoveSystem;
    float m_sensitivity{0.01f};
    float m_move_speed{1.0f}; // max speed in m/s
    float m_acceleration{1.0f}; // m/s/s
    float m_deceleration{5.0f}; // m/s/s
    glm::vec3 m_current_velocity{0.0f, 0.0f, 0.0f}; // m/s {+x, +y, +z} world space
    float m_yaw{0.0f};   // along Z axis
    float m_pitch{glm::half_pi<float>()}; // along X axis
public:
    MouseMoveComponent& setSensitivity(float sensitivity)
    {
        m_sensitivity = sensitivity;
        return *this;
    }
    MouseMoveComponent& setMoveSpeed(float move_speed)
    {
        m_move_speed = move_speed;
        return *this;
    }
    MouseMoveComponent& setAcceleration(float acceleration)
    {
        m_acceleration = acceleration;
        return *this;
    }
    MouseMoveComponent& setDeceleration(float decleration)
    {
        m_deceleration = decleration;
        return *this;
    }
};

class MouseMoveSystem : public gc::System {

public:
    MouseMoveSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};