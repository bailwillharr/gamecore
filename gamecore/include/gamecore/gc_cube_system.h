#pragma once

#include <vector>
#include <span>
#include <mat4x4.hpp>

#include "gamecore/gc_ecs.h"

namespace gc {

class World; // forward-dec

class CubeSystem : public System {

public:
    CubeSystem(gc::World& world);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc