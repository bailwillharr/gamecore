#pragma once

#include "gamecore/gc_ecs.h"

namespace gc {

class World;      // forward-dec
struct FrameState; // forward-dec

class CubeSystem : public System {

public:
    CubeSystem(World& world);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc
