#include "visuals.h"

#include <cmath>

#include <vector>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_world.h>

#include "character.h"
#include "components.h"

VisualsSystem::VisualsSystem(gc::World& world) : gc::System(world) {}

void VisualsSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    const float delta_time = static_cast<float>(frame_state.delta_time);
    m_world.forEach<gc::TransformComponent, SpinComponent>([&](gc::Entity, gc::TransformComponent& t, SpinComponent& spin) {
        if (!spin.started) {
            spin.base_z = t.getPosition().z;
            spin.started = true;
        }
        spin.angle = std::fmod(spin.angle + spin.radians_per_second * delta_time, glm::two_pi<float>());
        t.setRotation(glm::angleAxis(spin.angle, spin.axis));
        if (spin.bob_height != 0.0f) {
            spin.bob_angle = std::fmod(spin.bob_angle + spin.bob_speed * delta_time, glm::two_pi<float>());
            glm::vec3 position = t.getPosition();
            position.z = spin.base_z + spin.bob_height * std::sin(spin.bob_angle);
            t.setPosition(position);
        }
    });

    updatePlayerModels();
}

// A player's transform is its camera, which pitches up and down. Its model should only turn left and right.
void VisualsSystem::updatePlayerModels()
{
    struct Update {
        gc::Entity model;
        glm::vec3 position;
        glm::quat rotation;
    };
    std::vector<Update> updates{};
    m_world.forEach<gc::TransformComponent, PlayerModelComponent>([&](gc::Entity, const gc::TransformComponent& t, const PlayerModelComponent& player_model) {
        float yaw{}, pitch{};
        getYawAndPitch(t.getRotation(), yaw, pitch);
        const glm::quat inverse_rotation = glm::inverse(t.getRotation());
        // cancel out the parent's rotation, then apply only the yaw. The model's origin is at its feet, and it faces -Y.
        const glm::quat upright = glm::angleAxis(glm::pi<float>() - yaw, glm::vec3{0.0f, 0.0f, 1.0f});
        updates.push_back(Update{player_model.model, inverse_rotation * glm::vec3{0.0f, 0.0f, -CHARACTER_EYE_HEIGHT}, inverse_rotation * upright});
    });
    for (const Update& update : updates) {
        gc::TransformComponent* const t = m_world.getComponent<gc::TransformComponent>(update.model);
        if (t && (t->getPosition() != update.position || t->getRotation() != update.rotation)) {
            t->setPosition(update.position);
            t->setRotation(update.rotation);
        }
    }
}
