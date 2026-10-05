#pragma once

// Draws the scoreboard, the local player's health, a crosshair (which flashes when a bolt hits) and who burned who.

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class ReplicationSystem; // forward-dec
}

class MatchSystem; // forward-dec

class HudSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("HudSystem");

private:
    gc::ReplicationSystem& m_replication;
    MatchSystem& m_match;

public:
    HudSystem(gc::World& world, gc::ReplicationSystem& replication, MatchSystem& match);

    void onUpdate(gc::FrameState& frame_state) override;
};
