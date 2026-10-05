#pragma once

// Lets the person at the keyboard control the local player: looking, walking, jumping and throwing bolts.
// Only registered on hosts with a window. (Bots are controlled by BotSystem instead.)

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

namespace gc {
class CollisionSystem; // forward-dec
}

class MatchSystem; // forward-dec

class PlayerControlSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("PlayerControlSystem");

private:
    gc::CollisionSystem& m_collision;
    MatchSystem& m_match;
    float m_sensitivity{2.5e-3f}; // radians per unit of mouse movement

public:
    PlayerControlSystem(gc::World& world, gc::CollisionSystem& collision, MatchSystem& match);

    void onUpdate(gc::FrameState& frame_state) override;
};
