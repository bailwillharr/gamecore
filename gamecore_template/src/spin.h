#pragma once

#include <vec3.hpp>
#include <geometric.hpp>

#include <gamecore/gc_ecs.h>

class SpinComponent {
    friend class SpinSystem;
    float m_angle_radians{};
    glm::vec3 m_axis_norm{0.0f, 1.0f, 0.0f};
    float m_radians_per_second{1.0f};

public:
    SpinComponent& setRadiansPerSecond(float radians_per_second)
    {
        m_radians_per_second = radians_per_second;
        return *this;
    }
    SpinComponent& setAxis(const glm::vec3& axis)
    {
        m_axis_norm = glm::normalize(axis);
        return *this;
    }
};

class SpinSystem : public gc::System {
public:
    SpinSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};