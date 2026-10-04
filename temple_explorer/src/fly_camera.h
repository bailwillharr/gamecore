#pragma once

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
};

class FlyCameraSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("FlyCameraSystem");

private:
    bool m_framed{false};

public:
    explicit FlyCameraSystem(gc::World& world);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    // Moves the cameras to where the whole world can be seen, and picks a flying speed that suits the size of the world.
    void frameWorld();

    void showHelp(const FlyCameraComponent& camera, const glm::vec3& position, bool headlamp);
};
