#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

namespace gc {

class World;       // forward-dec
struct FrameState; // forward-dec

class LightSystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("LightSystem");

public:
    LightSystem(World& world);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc
