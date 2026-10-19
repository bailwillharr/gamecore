#include "explorer.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "components.h"

static constexpr float RADIUS = 0.3f;
static constexpr float HEIGHT = 1.75f;
static constexpr float WALK_SPEED = 4.0f;      // m/s
static constexpr float RUN_SPEED = 10.0f;      // m/s, holding shift
static constexpr float JUMP_SPEED = 3.2f;      // m/s: about 1.4 m high, in Mars's gravity
static constexpr float GROUND_ACCELERATION = 30.0f;
static constexpr float AIR_ACCELERATION = 4.0f;
static constexpr float WALKABLE_NORMAL_Z = 0.7f; // ground steeper than about 45 degrees is a wall
static constexpr float MAX_STEP = 0.2f;          // metres moved between collision checks, less than the radius
static constexpr float FLY_SPEED = 40.0f;        // m/s. Shift: 10 times faster, control: 10 times slower
static constexpr float FLY_CLEARANCE = 1.0f;     // a flying explorer's feet stay this far above the ground

glm::quat ExplorerSystem::makeLookRotation(float yaw, float pitch)
{
    // A camera looks along its -Z axis, which is straight down when it isn't rotated: tilt it up to the horizon, then turn it.
    return glm::angleAxis(-yaw, glm::vec3{0.0f, 0.0f, 1.0f}) * glm::angleAxis(glm::half_pi<float>() + pitch, glm::vec3{1.0f, 0.0f, 0.0f});
}

glm::vec3 ExplorerSystem::getLookDirection(float yaw, float pitch)
{
    return glm::vec3{std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
}

// Pushes the explorer out of whatever they are touching. Returns true if they are standing on something.
static bool resolveCollisions(const gc::CollisionSystem& collision, gc::Entity self, glm::vec3& feet, glm::vec3& velocity)
{
    // the centres of the spheres, above the feet. The lowest one is what stands on the ground
    constexpr std::array<float, 3> SPHERE_HEIGHTS{RADIUS, 0.5f * HEIGHT, HEIGHT - RADIUS};

    bool grounded = false;
    std::vector<gc::SphereContact> contacts{};
    // Moving out of one thing can move the explorer into another, so look again a few times
    for (int iteration = 0; iteration < 4; ++iteration) {
        bool touching = false;
        for (size_t i = 0; i < SPHERE_HEIGHTS.size(); ++i) {
            contacts.clear();
            collision.overlapSphere(feet + glm::vec3{0.0f, 0.0f, SPHERE_HEIGHTS[i]}, RADIUS, contacts, self);
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
                // Ground. Moving straight up, rather than along the normal, stops the explorer creeping down slopes
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

ExplorerSystem::ExplorerSystem(gc::World& world, gc::CollisionSystem& collision, const gc::ResourceTerrain& terrain, gc::Entity explorer,
                               const glm::vec3& spawn_feet, float spawn_yaw)
    : gc::System(world), m_collision(collision), m_terrain(terrain), m_explorer(explorer), m_spawn_feet(spawn_feet), m_spawn_yaw(spawn_yaw)
{
}

void ExplorerSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    ExplorerComponent* const explorer = m_world.getComponent<ExplorerComponent>(m_explorer);
    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(m_explorer);
    if (!explorer || !transform || !frame_state.window_state) {
        return;
    }
    const gc::WindowState& input = *frame_state.window_state;
    // (a long frame, while loading, shouldn't throw the explorer across the map)
    const float delta_time = std::clamp(static_cast<float>(frame_state.delta_time), 0.0f, 0.1f);
    const glm::vec3 eye_offset{0.0f, 0.0f, EXPLORER_EYE_HEIGHT};
    glm::vec3 feet = transform->getPosition() - eye_offset;
    const glm::vec3 start = feet;

    // The mouse isn't captured while the debug windows are open (F10). The explorer keeps falling, but ignores the keys
    const bool has_input = input.getIsMouseCaptured();
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    if (has_input) {
        const glm::vec2 mouse_motion = input.getMouseMotion();
        constexpr float MAX_PITCH = glm::half_pi<float>() - 0.01f;
        explorer->yaw += mouse_motion.x * m_sensitivity;
        explorer->pitch = std::clamp(explorer->pitch + mouse_motion.y * m_sensitivity, -MAX_PITCH, MAX_PITCH);
        forward += input.getKeyDown(SDL_SCANCODE_W) ? 1.0f : 0.0f;
        forward -= input.getKeyDown(SDL_SCANCODE_S) ? 1.0f : 0.0f;
        right += input.getKeyDown(SDL_SCANCODE_D) ? 1.0f : 0.0f;
        right -= input.getKeyDown(SDL_SCANCODE_A) ? 1.0f : 0.0f;
        up += (input.getKeyDown(SDL_SCANCODE_SPACE) || input.getKeyDown(SDL_SCANCODE_E)) ? 1.0f : 0.0f;
        up -= input.getKeyDown(SDL_SCANCODE_Q) ? 1.0f : 0.0f;

        if (input.getKeyPress(SDL_SCANCODE_F)) {
            explorer->flying = !explorer->flying;
            explorer->velocity = glm::vec3{0.0f};
        }
        if (input.getKeyPress(SDL_SCANCODE_R)) {
            // back to the start
            feet = m_spawn_feet;
            explorer->yaw = m_spawn_yaw;
            explorer->pitch = 0.0f;
            explorer->velocity = glm::vec3{0.0f};
            explorer->flying = false;
        }
    }
    const bool fast = has_input && (input.getKeyDown(SDL_SCANCODE_LSHIFT) || input.getKeyDown(SDL_SCANCODE_RSHIFT));
    const bool slow = has_input && (input.getKeyDown(SDL_SCANCODE_LCTRL) || input.getKeyDown(SDL_SCANCODE_RCTRL));

    if (explorer->flying) {
        // Where the explorer is looking, up and down too
        glm::vec3 wish = forward * getLookDirection(explorer->yaw, explorer->pitch) + right * getLookDirection(explorer->yaw + glm::half_pi<float>(), 0.0f) +
                         up * glm::vec3{0.0f, 0.0f, 1.0f};
        if (const float length = glm::length(wish); length > 1.0f) {
            wish /= length;
        }
        const float speed = FLY_SPEED * (fast ? 10.0f : 1.0f) * (slow ? 0.1f : 1.0f);
        // (eased, so that it doesn't jerk)
        explorer->velocity = glm::mix(explorer->velocity, wish * speed, std::min(1.0f, 8.0f * delta_time));
        feet += explorer->velocity * delta_time;
        if (const std::optional<float> ground = m_terrain.getHeight(feet.x, feet.y); ground && feet.z < *ground + FLY_CLEARANCE) {
            feet.z = *ground + FLY_CLEARANCE;
            explorer->velocity.z = std::max(explorer->velocity.z, 0.0f);
        }
        explorer->grounded = false;
    }
    else {
        // steer the horizontal velocity towards what is wanted. W and S walk the way the explorer faces, whether they look up or down
        glm::vec3 wish = forward * getLookDirection(explorer->yaw, 0.0f) + right * getLookDirection(explorer->yaw + glm::half_pi<float>(), 0.0f);
        if (const float length = glm::length(wish); length > 1.0f) {
            wish /= length;
        }
        const glm::vec3 wanted_velocity = wish * (fast ? RUN_SPEED : WALK_SPEED);
        glm::vec3 horizontal_velocity{explorer->velocity.x, explorer->velocity.y, 0.0f};
        const glm::vec3 difference = wanted_velocity - horizontal_velocity;
        const float max_change = (explorer->grounded ? GROUND_ACCELERATION : AIR_ACCELERATION) * delta_time;
        if (const float length = glm::length(difference); length > max_change) {
            horizontal_velocity += difference * (max_change / length);
        }
        else {
            horizontal_velocity = wanted_velocity;
        }
        explorer->velocity.x = horizontal_velocity.x;
        explorer->velocity.y = horizontal_velocity.y;
        if (explorer->grounded && has_input && input.getKeyDown(SDL_SCANCODE_SPACE)) {
            explorer->velocity.z = JUMP_SPEED;
            explorer->grounded = false;
        }
        explorer->velocity.z -= MARS_GRAVITY * delta_time;

        // In several small steps if moving fast, as there are no swept collision queries
        const float distance = glm::length(explorer->velocity) * delta_time;
        const int steps = std::clamp(static_cast<int>(std::ceil(distance / MAX_STEP)), 1, 64);
        const float step_time = delta_time / static_cast<float>(steps);
        bool grounded = false;
        for (int step = 0; step < steps; ++step) {
            feet += explorer->velocity * step_time;
            grounded = resolveCollisions(m_collision, m_explorer, feet, explorer->velocity);
        }
        explorer->grounded = grounded;

        if (grounded) {
            explorer->last_safe_feet = feet;
            explorer->has_safe_feet = true;
        }
        else if (!m_terrain.getHeight(feet.x, feet.y) && feet.z < m_terrain.header.min_height - 100.0f) {
            // Walked off the edge of the terrain and fell. Go back to where there was ground
            feet = explorer->has_safe_feet ? explorer->last_safe_feet + glm::vec3{0.0f, 0.0f, 0.5f} : m_spawn_feet;
            explorer->velocity = glm::vec3{0.0f};
        }
    }

    m_speed = (delta_time > 0.0f) ? glm::length(feet - start) / delta_time : 0.0f;
    transform->setPosition(feet + eye_offset);
    transform->setRotation(makeLookRotation(explorer->yaw, explorer->pitch));
}
