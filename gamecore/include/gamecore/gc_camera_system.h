#pragma once

#include <ext/matrix_clip_space.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_window.h>

namespace gc {

class CameraSystem : public System {
public:
    CameraSystem(World& world) : System(world) {}

    void onUpdate(FrameState& frame_state) override
    {
        const float aspect_ratio =
            static_cast<float>(frame_state.window_state->getWindowSize().x) / static_cast<float>(frame_state.window_state->getWindowSize().y);
        m_world.forEach<TransformComponent, CameraComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t, CameraComponent& c) {
            if (c.m_active) {
                // in view spacew
                const glm::mat4 projection_matrix = glm::perspectiveRH_ZO(c.m_fov_radians, aspect_ratio, c.m_near, c.m_far) * glm::scale(glm::mat4{1.0f}, glm::vec3{1.0f, -1.0f, 1.0f});
                frame_state.draw_data.setProjectionMatrix(projection_matrix);
                frame_state.draw_data.setViewMatrix(glm::inverse(t.getWorldMatrix()));
            }
        });
    }
};

} // namespace gc