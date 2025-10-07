#pragma once

#include <gamecore/gc_ecs.h>

class MouseMoveSystem; // forward-dec

class MouseMoveComponent {
    friend class MouseMoveSystem;
    float m_sensitivity{0.01f};
    float m_move_speed{1.0f};
    float m_yaw{0.0f};   // along Z axis
    float m_pitch{0.0f}; // along X axis
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
};

class MouseMoveSystem : public gc::System {

public:
    MouseMoveSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};