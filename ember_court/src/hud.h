#pragma once

// Shows the scoreboard, the local player's health, a crosshair (which flashes when a bolt hits) and who burned who.
// They are drawn by the engine's UI from content/ui/ember_court/ember_court_hud.rml, which an entity of the HUD's own shows (with a
// UIComponent). This system fills in the document's data model ("ember_hud") each frame.

#include <string>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class ReplicationSystem; // forward-dec
class UI;                // forward-dec
} // namespace gc

class MatchSystem; // forward-dec

class HudSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("HudSystem");

    // A line of the scoreboard
    struct ScoreRow {
        std::string name;
        int score;
        int deaths;
        bool local;
        bool operator==(const ScoreRow&) const = default;
    };

    // A line of the kill feed
    struct KillRow {
        std::string killer;
        std::string victim;
        float opacity;
        bool operator==(const KillRow&) const = default;
    };

private:
    gc::UI& m_ui;
    gc::ReplicationSystem& m_replication;
    MatchSystem& m_match;
    gc::Entity m_hud{gc::ENTITY_NONE}; // has the UIComponent

    // The data model, and the variables that are bound to it
    Rml::DataModelHandle m_model{};
    bool m_waiting{true};
    std::vector<ScoreRow> m_rows{};
    std::vector<KillRow> m_kills{};
    bool m_alive{};
    int m_health{};
    float m_health_fraction{};
    int m_score{};
    bool m_hit{};

public:
    HudSystem(gc::World& world, gc::UI& ui, gc::ReplicationSystem& replication, MatchSystem& match);
    HudSystem(const HudSystem&) = delete;

    ~HudSystem() override;

    HudSystem& operator=(const HudSystem&) = delete;

    void onUpdate(gc::FrameState& frame_state) override;
};
