#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_render_object_manager.h"

namespace gc {

class World;      // forward-dec
struct FrameState; // forward-dec

class RenderSystem : public System {
    RenderObjectManager m_render_object_manager;

public:
    RenderSystem(World& world, ResourceManager& resource_manager, RenderBackend& render_backend);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc
