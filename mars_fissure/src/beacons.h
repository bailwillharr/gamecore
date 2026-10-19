#pragma once

// The survey beacons: glowing masts at places worth seeing (the bottom of the fissure, the highest ground, and some others
// spread over the scan), to be walked or flown up to. Each one turns green when it is reached.

#include <string>
#include <vector>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

#include "components.h"
#include "game.h"

namespace gc {
class ResourceManager; // forward-dec
}

class BeaconSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("BeaconSystem");

    static constexpr float REACH_DISTANCE = 6.0f; // metres from the mast

private:
    gc::Entity m_explorer;
    double m_time{};
    uint32_t m_found{};
    uint32_t m_total{};
    double m_completed_after{}; // seconds, once they have all been found

public:
    // Makes the beacons' meshes and materials (resources of the game's own) and the beacons themselves
    BeaconSystem(gc::World& world, gc::ResourceManager& resource_manager, const Sites& sites, gc::Entity explorer);

    void onUpdate(gc::FrameState& frame_state) override;

    uint32_t getFoundCount() const { return m_found; }
    uint32_t getTotalCount() const { return m_total; }
    double getTime() const { return m_time; }
    double getCompletedAfter() const { return m_completed_after; } // zero until they have all been found

    static std::string getBeaconName(const BeaconComponent& beacon);
};
