#pragma once

#include <unordered_map>
#include <tuple>
#include <vector>

#include <glm/mat4x4.hpp>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_render_object_manager.h"

namespace gc {

class World;      // forward-dec
struct FrameState; // forward-dec

class RenderSystem : public System {
    struct MeshMaterialPairHash {
        size_t operator()(const std::pair<RenderMesh*, RenderMaterial*>& p) const noexcept
        {
            size_t h1 = std::hash<RenderMesh*>{}(p.first);
            size_t h2 = std::hash<RenderMaterial*>{}(p.second);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
        }
    };

    RenderObjectManager m_render_object_manager;
    std::unordered_map<std::pair<RenderMesh*, RenderMaterial*>, std::vector<glm::mat4>, MeshMaterialPairHash> m_instance_groups;

public:
    RenderSystem(World& world, ResourceManager& resource_manager, RenderBackend& render_backend);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc

