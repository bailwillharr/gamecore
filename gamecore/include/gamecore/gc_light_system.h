#pragma once

#include "gamecore/gc_ecs.h"

namespace gc {

class World;       // forward-dec
struct FrameState; // forward-dec

class LightSystem : public System {

public:
    LightSystem(World& world);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc
