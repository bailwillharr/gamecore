#pragma once

// Shows every player's score and health, and a crosshair.

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class ReplicationSystem; // forward-dec
}

class HudSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("HudSystem");

private:
    gc::ReplicationSystem& m_replication;

public:
    HudSystem(gc::World& world, gc::ReplicationSystem& replication);

    void onUpdate(gc::FrameState& frame_state) override;
};
