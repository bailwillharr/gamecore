#pragma once

#include <ext/matrix_clip_space.hpp>

#include <tracy/Tracy.hpp>

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
        ZoneScoped;

        bool has_camera = false;

        const float aspect_ratio =
            static_cast<float>(frame_state.window_state->getWindowSize().x) / static_cast<float>(frame_state.window_state->getWindowSize().y);
        m_world.forEach<TransformComponent, CameraComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const CameraComponent& c) {
            if (c.m_active) {
                // in view space
                glm::mat4 projection_matrix = glm::infinitePerspectiveRH_NO(c.m_fov_radians, aspect_ratio, c.m_near);

                projection_matrix[2][2] = 0.0f;
                projection_matrix[3][2] = c.m_near; // push near to depth = 1

                projection_matrix[1][1] *= -1.0f;

                frame_state.draw_data.setProjectionMatrix(projection_matrix);
                frame_state.draw_data.setViewMatrix(glm::inverse(t.getWorldMatrix()));

                has_camera = true;
            }
        });

        if (!has_camera) {
            GC_ERROR("No camera in world");
        }
    }
};

} // namespace gc
