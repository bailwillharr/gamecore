#include "player_control.h"

#include <algorithm>

#include <glm/gtc/constants.hpp>
#include <glm/vec3.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "character.h"
#include "match.h"

PlayerControlSystem::PlayerControlSystem(gc::World& world, gc::CollisionSystem& collision, MatchSystem& match)
    : gc::System(world), m_collision(collision), m_match(match)
{
}

void PlayerControlSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    const gc::Entity player = m_match.getLocalPlayer();
    if (!frame_state.window_state || player == gc::ENTITY_NONE) {
        return;
    }
    CharacterComponent* const character = m_world.getComponent<CharacterComponent>(player);
    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(player);
    if (!character || !transform) {
        return;
    }
    const gc::WindowState& input = *frame_state.window_state;

    if (!character->facing_known) {
        // The player has just been spawned, or put somewhere else by the server. Face the way it was put.
        getYawAndPitch(transform->getRotation(), character->yaw, character->pitch);
        character->facing_known = true;
    }

    if (m_match.isFrozen()) {
        return; // nothing may change, not even by falling
    }

    // The mouse isn't captured while the debug windows are open (F10). The player keeps falling, but ignores the keys.
    const bool has_input = input.getIsMouseCaptured();

    CharacterInput character_input{};
    if (has_input) {
        const glm::vec2 mouse_motion = input.getMouseMotion();
        constexpr float MAX_PITCH = glm::half_pi<float>() - 0.01f;
        character->yaw += mouse_motion.x * m_sensitivity;
        character->pitch = std::clamp(character->pitch + mouse_motion.y * m_sensitivity, -MAX_PITCH, MAX_PITCH);

        float forward = 0.0f;
        float right = 0.0f;
        forward += input.getKeyDown(SDL_SCANCODE_W) ? 1.0f : 0.0f;
        forward -= input.getKeyDown(SDL_SCANCODE_S) ? 1.0f : 0.0f;
        right += input.getKeyDown(SDL_SCANCODE_D) ? 1.0f : 0.0f;
        right -= input.getKeyDown(SDL_SCANCODE_A) ? 1.0f : 0.0f;
        // W and S walk the way the player is facing, whether they are looking up or down
        character_input.wish_direction =
            forward * getLookDirection(character->yaw, 0.0f) + right * getLookDirection(character->yaw + glm::half_pi<float>(), 0.0f);
        character_input.jump = input.getKeyDown(SDL_SCANCODE_SPACE);
    }

    // A player's transform is its eyes: it has the camera on it.
    const glm::vec3 eye_offset{0.0f, 0.0f, CHARACTER_EYE_HEIGHT};
    const glm::vec3 feet =
        moveCharacter(m_collision, *character, transform->getPosition() - eye_offset, character_input, static_cast<float>(frame_state.delta_time));
    transform->setPosition(feet + eye_offset);
    transform->setRotation(makeLookRotation(character->yaw, character->pitch));

    if (has_input && input.getButtonDown(gc::MouseButton::LEFT)) {
        m_match.requestFire(getLookDirection(character->yaw, character->pitch));
    }
}
