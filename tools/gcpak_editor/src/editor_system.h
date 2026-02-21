#pragma once

#include <gamecore/gc_ecs.h>

class EditorSystem : public gc::System {
public:
    EditorSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};
