#pragma once

// Things that move only for show, which every host that renders does for itself: spinning and bobbing (SpinComponent, used by
// decorations in the map and by pickups), and keeping the models of other players upright.

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

class VisualsSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("VisualsSystem");

public:
    explicit VisualsSystem(gc::World& world);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    void updatePlayerModels();
};
