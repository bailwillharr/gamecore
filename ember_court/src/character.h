#pragma once

// A walking player: gravity, jumping, and not walking through the map.
//
// The engine's CollisionSystem doesn't move anything, it only answers questions, so this is the game's own character controller,
// built on CollisionSystem::overlapSphere(). A character is three spheres on top of each other. After every step they are pushed
// out of whatever they ended up in. Ground that is flat enough to stand on pushes straight up, so that characters don't slide
// down ramps.
//
// It is used for the local player and for bots. Players controlled by other hosts are just shown where their host says they are.

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "components.h"

namespace gc {
class CollisionSystem; // forward-dec
}

inline constexpr float CHARACTER_RADIUS = 0.35f;
inline constexpr float CHARACTER_HEIGHT = 1.75f;
inline constexpr float CHARACTER_EYE_HEIGHT = 1.6f; // A player's transform is its eyes (its camera). Its feet are this far below
inline constexpr float CHARACTER_SPEED = 6.5f;      // m/s
inline constexpr float CHARACTER_JUMP_SPEED = 6.0f; // m/s. Enough to get onto a 1 m crate
inline constexpr float CHARACTER_GRAVITY = 16.0f;   // m/s^2

struct CharacterInput {
    glm::vec3 wish_direction{}; // where the player wants to go: horizontal, in world space, no longer than 1
    bool jump{false};
};

// The rotation of a camera that looks in the direction of yaw and pitch (see CharacterComponent)
glm::quat makeLookRotation(float yaw, float pitch);
glm::vec3 getLookDirection(float yaw, float pitch);
void getYawAndPitch(const glm::quat& rotation, float& yaw, float& pitch);

// Moves a character for one frame. 'feet' is the bottom of the character. Returns where its feet end up.
glm::vec3 moveCharacter(const gc::CollisionSystem& collision, CharacterComponent& character, glm::vec3 feet, const CharacterInput& input, float delta_time);
