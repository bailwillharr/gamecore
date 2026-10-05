#include "gamecore/gc_light_system.h"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_world.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_light_component.h"
#include "gamecore/gc_frame_state.h"

namespace gc {

LightSystem::LightSystem(gc::World& world) : gc::System(world) {}

void LightSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    m_world.forEach<TransformComponent, LightComponent>([&]([[maybe_unused]] Entity entity, const TransformComponent& t, const LightComponent& l) {
        const glm::vec3 color = l.m_color * l.m_intensity;
        if (l.m_type == LightType::DIRECTIONAL) {
            // The light shines along the entity's -Z axis, so its +Z axis points towards the light.
            // Only one directional light is drawn: the last one found.
            const glm::vec3 z_axis = glm::vec3(t.getWorldMatrix()[2]);
            if (glm::dot(z_axis, z_axis) > 0.0f) {
                frame_state.draw_data.setDirectionalLight(glm::normalize(z_axis), color);
            }
        }
        else if (l.m_type == LightType::AMBIENT) {
            frame_state.draw_data.setAmbientLight(frame_state.draw_data.getAmbientLight() + color);
        }
        else {
            frame_state.draw_data.addPointLight(t.getWorldPosition(), l.m_range, color);
        }
    });
}

} // namespace gc
