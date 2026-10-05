#include "character.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <gamecore/gc_collision_system.h>

static constexpr float GROUND_ACCELERATION = 60.0f; // m/s^2
static constexpr float AIR_ACCELERATION = 12.0f;    // m/s^2. Less control while in the air
static constexpr float WALKABLE_NORMAL_Z = 0.7f;    // ground steeper than about 45 degrees is a wall
static constexpr float MAX_STEP = 0.2f;             // metres moved between collision checks. Less than the radius, so nothing is skipped
static constexpr float FALL_LIMIT_Z = -30.0f;       // a character that falls this far has left the map

glm::quat makeLookRotation(float yaw, float pitch)
{
    // A camera looks along its -Z axis, which is straight down when it isn't rotated: tilt it up to the horizon, then turn it.
    return glm::angleAxis(-yaw, glm::vec3{0.0f, 0.0f, 1.0f}) * glm::angleAxis(glm::half_pi<float>() + pitch, glm::vec3{1.0f, 0.0f, 0.0f});
}

glm::vec3 getLookDirection(float yaw, float pitch) { return glm::vec3{std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)}; }

void getYawAndPitch(const glm::quat& rotation, float& yaw, float& pitch)
{
    const glm::vec3 forward = rotation * glm::vec3{0.0f, 0.0f, -1.0f};
    yaw = std::atan2(forward.x, forward.y);
    pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
}

// Pushes the character out of whatever it is touching. Returns true if it is standing on something.
static bool resolveCollisions(const gc::CollisionSystem& collision, glm::vec3& feet, glm::vec3& velocity)
{
    // the centres of the spheres, above the feet. The lowest one is what stands on the ground
    constexpr std::array<float, 3> SPHERE_HEIGHTS{CHARACTER_RADIUS, 0.5f * CHARACTER_HEIGHT, CHARACTER_HEIGHT - CHARACTER_RADIUS};

    bool grounded = false;
    std::vector<gc::SphereContact> contacts{};
    // Moving out of one thing can move the character into another (in a corner), so look again a few times.
    for (int iteration = 0; iteration < 4; ++iteration) {
        bool touching = false;
        for (size_t i = 0; i < SPHERE_HEIGHTS.size(); ++i) {
            contacts.clear();
            collision.overlapSphere(feet + glm::vec3{0.0f, 0.0f, SPHERE_HEIGHTS[i]}, CHARACTER_RADIUS, contacts);
            const gc::SphereContact* deepest = nullptr;
            for (const gc::SphereContact& contact : contacts) {
                if (!deepest || contact.depth > deepest->depth) {
                    deepest = &contact;
                }
            }
            if (!deepest || deepest->depth <= 0.0f) {
                continue;
            }
            touching = true;
            if (i == 0 && deepest->normal.z > WALKABLE_NORMAL_Z) {
                // Ground. Moving straight up, rather than along the normal, stops the character creeping down slopes.
                feet.z += deepest->depth / deepest->normal.z;
                velocity.z = std::max(velocity.z, 0.0f);
                grounded = true;
            }
            else {
                feet += deepest->normal * deepest->depth;
                // keep the part of the velocity that slides along the surface
                const float into_surface = glm::dot(velocity, deepest->normal);
                if (into_surface < 0.0f) {
                    velocity -= deepest->normal * into_surface;
                }
            }
        }
        if (!touching) {
            break;
        }
    }
    return grounded;
}

glm::vec3 moveCharacter(const gc::CollisionSystem& collision, CharacterComponent& character, glm::vec3 feet, const CharacterInput& input, float delta_time)
{
    delta_time = std::clamp(delta_time, 0.0f, 0.1f); // a long frame (e.g. while loading) shouldn't throw the character across the map

    // steer the horizontal velocity towards what is wanted
    glm::vec3 wish = glm::vec3{input.wish_direction.x, input.wish_direction.y, 0.0f};
    if (const float length = glm::length(wish); length > 1.0f) {
        wish /= length;
    }
    const glm::vec3 wanted_velocity = wish * CHARACTER_SPEED;
    glm::vec3 horizontal_velocity{character.velocity.x, character.velocity.y, 0.0f};
    const glm::vec3 difference = wanted_velocity - horizontal_velocity;
    const float max_change = (character.grounded ? GROUND_ACCELERATION : AIR_ACCELERATION) * delta_time;
    if (const float length = glm::length(difference); length > max_change) {
        horizontal_velocity += difference * (max_change / length);
    }
    else {
        horizontal_velocity = wanted_velocity;
    }
    character.velocity.x = horizontal_velocity.x;
    character.velocity.y = horizontal_velocity.y;

    if (character.grounded && input.jump) {
        character.velocity.z = CHARACTER_JUMP_SPEED;
        character.grounded = false;
    }
    character.velocity.z -= CHARACTER_GRAVITY * delta_time;

    // In several small steps if it is moving fast, as there are no swept collision queries
    const float distance = glm::length(character.velocity) * delta_time;
    const int steps = std::clamp(static_cast<int>(std::ceil(distance / MAX_STEP)), 1, 16);
    const float step_time = delta_time / static_cast<float>(steps);
    bool grounded = false;
    for (int step = 0; step < steps; ++step) {
        feet += character.velocity * step_time;
        grounded = resolveCollisions(collision, feet, character.velocity);
    }
    character.grounded = grounded;

    // Fell out of the world? (There shouldn't be a way to, but the map is data.) Go back to where there was ground.
    if (grounded) {
        character.last_safe_feet = feet;
        character.has_safe_feet = true;
    }
    else if (feet.z < FALL_LIMIT_Z && character.has_safe_feet) {
        feet = character.last_safe_feet + glm::vec3{0.0f, 0.0f, 0.5f};
        character.velocity = glm::vec3{0.0f};
    }

    return feet;
}
