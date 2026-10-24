#pragma once

// What is drawn over the world: where the explorer is, the beacons (and markers over them), the controls, and how the terrain is
// doing. They are drawn by the engine's UI from content/ui/mars_fissure/mars_hud.rml, which an entity of the HUD's own shows (with
// a UIComponent). This system fills in the document's data model ("mars_hud") each frame.
// Also the TerrainSystem's debug window (F10, Windows > Terrain, with ImGui), and the keys that change how the terrain is drawn.

#include <string>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class DebugUI;          // forward-dec
class TerrainSystem;    // forward-dec
class UI;               // forward-dec
struct ResourceTerrain; // forward-dec
} // namespace gc

class ExplorerSystem; // forward-dec
class BeaconSystem;   // forward-dec

class HudSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("HudSystem");

    // A marker over a beacon, in pixels from the top left of the screen
    struct Marker {
        float x;
        float y;
        std::string label;
        bool found;
        bool operator==(const Marker&) const = default;
    };

private:
    gc::UI& m_ui;
    gc::DebugUI& m_debug_ui;
    gc::TerrainSystem& m_terrain_system;
    const gc::ResourceTerrain& m_terrain;
    const ExplorerSystem* m_explorer_system; // null during a flythrough
    const BeaconSystem& m_beacons;
    gc::Entity m_explorer;
    gc::Entity m_hud{gc::ENTITY_NONE}; // has the UIComponent

    // The data model, and the variables that are bound to it
    Rml::DataModelHandle m_model{};
    bool m_has_explorer{};
    bool m_show_help{true};
    std::string m_mode{};
    std::string m_speed{};
    std::string m_position{};
    std::string m_elevation{};
    int m_found{};
    int m_total{};
    float m_found_fraction{};
    bool m_has_nearest{};
    std::string m_nearest{};
    bool m_complete{};
    std::string m_complete_text{};
    std::string m_fps{};
    std::string m_patches{};
    std::string m_tiles{};
    std::string m_streamed{};
    std::string m_detail{};
    bool m_detail_warning{};
    std::vector<Marker> m_markers{};

public:
    HudSystem(gc::World& world, gc::UI& ui, gc::DebugUI& debug_ui, gc::TerrainSystem& terrain_system, const gc::ResourceTerrain& terrain,
              const ExplorerSystem* explorer_system, const BeaconSystem& beacons, gc::Entity explorer);
    HudSystem(const HudSystem&) = delete;

    ~HudSystem() override;

    HudSystem& operator=(const HudSystem&) = delete;

    void onUpdate(gc::FrameState& frame_state) override;
};
