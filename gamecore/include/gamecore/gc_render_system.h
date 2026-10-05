#pragma once

#include <memory>
#include <unordered_map>
#include <tuple>
#include <vector>

#include <glm/mat4x4.hpp>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_render_object_manager.h"
#include "gamecore/gc_render_shadow_map.h"

namespace gc {

class World;       // forward-dec
struct FrameState; // forward-dec

class RenderSystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("RenderSystem");

private:
    struct MeshMaterialPairHash {
        size_t operator()(const std::pair<RenderMesh*, RenderMaterial*>& p) const noexcept
        {
            size_t h1 = std::hash<RenderMesh*>{}(p.first);
            size_t h2 = std::hash<RenderMaterial*>{}(p.second);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
        }
    };

    ResourceManager& m_resource_manager;
    RenderBackend& m_render_backend;

    RenderObjectManager m_render_object_manager;

    // The shadow map of the world's ShadowMapComponent. It is kept until a different one is wanted.
    Name m_shadow_map_name{};
    std::unique_ptr<RenderShadowMap> m_shadow_map{}; // null if m_shadow_map_name couldn't be loaded
    std::unordered_map<std::pair<RenderMesh*, RenderMaterial*>, std::vector<glm::mat4>, MeshMaterialPairHash> m_instance_groups;

public:
    RenderSystem(World& world, ResourceManager& resource_manager, RenderBackend& render_backend);

    void onUpdate(FrameState& frame_state) override;
};

} // namespace gc
