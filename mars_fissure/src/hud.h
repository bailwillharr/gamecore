#pragma once

// What is drawn over the world, with ImGui: where the explorer is, the beacons (and markers over them), the controls, and how the
// terrain is doing. Also the TerrainSystem's debug window (F10, Windows > Terrain), and the keys that change how the terrain is
// drawn.

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class DebugUI;          // forward-dec
class TerrainSystem;    // forward-dec
struct ResourceTerrain; // forward-dec
} // namespace gc

class ExplorerSystem; // forward-dec
class BeaconSystem;   // forward-dec

class HudSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("HudSystem");

private:
    gc::DebugUI& m_debug_ui;
    gc::TerrainSystem& m_terrain_system;
    const gc::ResourceTerrain& m_terrain;
    const ExplorerSystem* m_explorer_system; // null during a flythrough
    const BeaconSystem& m_beacons;
    gc::Entity m_explorer;
    bool m_show_help{true};

public:
    HudSystem(gc::World& world, gc::DebugUI& debug_ui, gc::TerrainSystem& terrain_system, const gc::ResourceTerrain& terrain,
              const ExplorerSystem* explorer_system, const BeaconSystem& beacons, gc::Entity explorer);

    void onUpdate(gc::FrameState& frame_state) override;
};
