#pragma once

#include <gamecore/gc_ecs.h>

class MouseMoveComponent {
public:
    float sensitivity{1.0f};
};

class MouseMoveSystem : public gc::System {

public:
    MouseMoveSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};