#pragma once

#include <string>

#include <glm/gtc/constants.hpp>
#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

// Put on the camera entity. Lets the player look around with the mouse and fly in the direction they are looking.
struct FlyCameraComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("FlyCameraComponent");

    float sensitivity{0.0025f};         // radians per unit of mouse motion
    float speed{5.0f};                  // m/s
    float fast_multiplier{4.0f};        // while shift is held
    float smoothing_time{0.1f};         // seconds taken to (mostly) reach the wanted velocity
    glm::vec3 velocity{};               // m/s, world space
    float yaw{0.0f};                    // about the Z axis. Zero looks along +Y
    float pitch{glm::half_pi<float>()}; // about the X axis. Zero looks straight down, pi looks straight up
    bool collide{true};                 // the camera is a sphere that can't go through the world's colliders (C turns it off)
    float radius{0.4f};                 // of that sphere, in metres
};

// Puts the point light that the player can turn on and off (L) on the camera entity. Add its CameraComponent first.
void addHeadlamp(gc::World& world, gc::Entity entity);

class FlyCameraSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("FlyCameraSystem");

private:
    bool m_framed{false};
    std::string m_looking_at{}; // what is in the middle of the screen, for the help text

public:
    explicit FlyCameraSystem(gc::World& world);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    // Moves the cameras to where the whole world can be seen, and picks a flying speed that suits the size of the world.
    void frameWorld();

    void showHelp(const FlyCameraComponent& camera, const glm::vec3& position, bool headlamp);

    // Returns where a camera that wants to be at 'position' can be: outside of everything solid. Stops 'velocity' going into what
    // it touches.
    glm::vec3 collideWithWorld(const FlyCameraComponent& camera, gc::Entity entity, glm::vec3 position, glm::vec3& velocity);
};
