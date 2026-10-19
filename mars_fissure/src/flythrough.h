#pragma once

// Flies the camera along a set route over the terrain, logging how long each frame took and what the terrain had to stream,
// then quits. For seeing how the terrain copes, without anyone at the keyboard: run with --flythrough, and syncmode=3 to measure
// without vsync. The route takes in the ground at walking height, low passes along the fissure at speed (hundreds of metres a
// second, so that tiles have to be streamed in quickly) and the whole terrain from high up.

#include <vector>

#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

#include "game.h"

namespace gc {
class App;              // forward-dec
class TerrainSystem;    // forward-dec
struct ResourceTerrain; // forward-dec
} // namespace gc

class FlythroughSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("FlythroughSystem");

private:
    struct Waypoint {
        glm::vec3 place; // x, y, and the height above the ground
        float seconds;   // to get here from the one before
    };

    gc::App& m_app;
    const gc::ResourceTerrain& m_terrain;
    gc::TerrainSystem& m_terrain_system;
    gc::Entity m_camera;
    std::vector<Waypoint> m_route{};
    float m_duration{};
    double m_time{};

    // this second's frames
    std::vector<float> m_second_frame_times{};
    uint32_t m_second_uploads{};
    uint32_t m_second_most_missing{};
    // all of them
    std::vector<float> m_frame_times{};
    uint32_t m_most_missing{};
    uint32_t m_most_patches{};
    uint64_t m_most_triangles{};
    uint32_t m_most_slots_used{};

public:
    FlythroughSystem(gc::World& world, gc::App& app, const gc::ResourceTerrain& terrain, gc::TerrainSystem& terrain_system, gc::Entity camera,
                     const Sites& sites, float seconds);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    glm::vec3 getPlace(double time) const; // where the camera is (in the world) at this time
    void logSecond();
    void logSummary();
};
