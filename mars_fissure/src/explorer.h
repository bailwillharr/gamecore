#pragma once

// Lets the person at the keyboard walk on the terrain, or fly over it.
//
// Walking uses a character controller like ember_court's: the explorer is three spheres on top of each other, pushed out of
// whatever they end up in by the CollisionSystem's sphere queries, in Mars's gravity (about 38% of Earth's). The terrain is the
// most detailed data at every point, 1 m apart, as is drawn near the camera. Ground steeper than about 45 degrees can't be
// walked up: the fissure's walls are cliffs.
//
// Flying ignores collisions, but doesn't go underground.

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class CollisionSystem;  // forward-dec
struct ResourceTerrain; // forward-dec
} // namespace gc

inline constexpr float MARS_GRAVITY = 3.71f;   // m/s^2
inline constexpr float EXPLORER_EYE_HEIGHT = 1.6f; // the explorer's transform is their eyes. Their feet are this far below

class ExplorerSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("ExplorerSystem");

private:
    gc::CollisionSystem& m_collision;
    const gc::ResourceTerrain& m_terrain;
    gc::Entity m_explorer;
    glm::vec3 m_spawn_feet;
    float m_spawn_yaw;
    float m_sensitivity{2.5e-3f}; // radians per unit of mouse movement
    float m_speed{};              // how fast the explorer went last frame, m/s

public:
    ExplorerSystem(gc::World& world, gc::CollisionSystem& collision, const gc::ResourceTerrain& terrain, gc::Entity explorer, const glm::vec3& spawn_feet,
                   float spawn_yaw);

    void onUpdate(gc::FrameState& frame_state) override;

    float getSpeed() const { return m_speed; }

    // The rotation of a camera that looks in the direction of yaw and pitch
    static glm::quat makeLookRotation(float yaw, float pitch);
    static glm::vec3 getLookDirection(float yaw, float pitch);
};
