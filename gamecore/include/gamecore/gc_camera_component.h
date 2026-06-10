#pragma once

#include <glm/trigonometric.hpp>

#include "gamecore/gc_name.h"

namespace gc {

class CameraSystem; // forward-dec

class CameraComponent {
    friend class CameraSystem;

public:
    static constexpr auto NAME = Name::createConstexpr("CameraComponent");

private:
    float m_fov_radians{glm::radians(45.0f)};
    float m_near = 0.1f;
    bool m_active = true;

public:
    CameraComponent& setFOV(float fov_radians)
    {
        m_fov_radians = fov_radians;
        return *this;
    }
    CameraComponent& setNearPlane(float near_plane)
    {
        m_near = near_plane;
        return *this;
    }
    CameraComponent& setActive(bool active)
    {
        m_active = active;
        return *this;
    }
};

} // namespace gc
