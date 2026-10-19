#pragma once

// Draws the world's terrains (TerrainComponent), streaming their tiles into GPU memory as the camera moves.
//
// Choosing what to draw (after CDLOD, Strugar 2009): every level of detail has a range, twice that of the level below it, starting
// with the component's detail distance for level 0. Going down the quadtree from the top, a node is split into its children if
// it is within the range of the level below. Otherwise it is drawn: as four patches, one for each quarter of its tile, so that a
// node can be drawn where some of its children aren't (because they are out of range, or not loaded yet). Patches outside the
// camera's view aren't drawn. So detailed tiles are drawn near the camera, coarser and coarser ones further away, and about the
// same number of triangles cover each part of the screen.
//
// Near the end of its range a patch turns into its parent (see content/shader_src/terrain.vert), so that switching between
// levels doesn't pop and neighbouring patches of different levels meet. Skirts hide any gaps that are left.
//
// Streaming: a node's tile has to be in the RenderTerrain's cache before the node can be drawn, and a node can only be split
// into children whose tiles are. Each frame, every node that is within range (or nearly) is marked as needed, whether or not
// it can be seen, so that turning round doesn't show a coarser terrain. Needed tiles that aren't in the cache are asked for, the
// coarsest and nearest first, and copied in by the render work thread from the mapped .gcpak file (so the disk is read then,
// not on the main thread). When the cache is full, the tiles that haven't been needed for longest are let go. Until a tile has
// arrived, its parent is drawn in its place.
//
// The TerrainSystem only draws: collisions with terrains are the CollisionSystem's. Register it after the CameraSystem, as it
// chooses patches for the camera that the frame will be drawn from.

#include <cstdint>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_render_object_manager.h"
#include "gamecore/gc_render_terrain.h"
#include "gamecore/gc_terrain.h"
#include "gamecore/gc_world_draw_data.h"

namespace gc {

class TerrainSystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("TerrainSystem");

    // Can be changed at any time, e.g. from the debug window
    struct Settings {
        float detail_scale = 1.0f;       // multiplies every terrain's detail distance
        bool show_levels = false;        // draw each level of detail in its own colour
        bool freeze = false;             // keep choosing patches for where the camera was when this was set
        uint32_t uploads_per_frame = 64; // tiles that may start being copied each frame
    };

    // What happened in the last frame, for all terrains together
    struct Stats {
        uint32_t terrains{};
        uint32_t patches{};          // drawn
        uint64_t triangles{};        // drawn, not counting skirts
        uint32_t slots{};            // in the tile caches
        uint32_t resident{};         // tiles that are in the caches
        uint32_t loading{};          // tiles being copied in
        uint32_t needed{};           // tiles that were needed
        uint32_t missing{};          // needed tiles that weren't in a cache, including those being copied in
        uint32_t uploads_started{};  // this frame
        uint32_t evictions{};        // this frame
        uint64_t total_uploads{};    // since the start
        uint64_t total_upload_bytes{};
        std::array<uint32_t, TERRAIN_MAX_LEVELS> patches_per_level{};
    };

private:
    enum class TileState : uint8_t { NONE, LOADING, RESIDENT };

    struct NodeState {
        uint32_t slot{TERRAIN_NO_NODE};
        uint64_t last_needed{};
        TileState state{TileState::NONE};
    };

    // One TerrainComponent's terrain, as it is drawn
    struct Instance {
        Name terrain{};
        const ResourceTerrain* resource{}; // null if it couldn't be loaded
        std::unique_ptr<RenderTerrain> render{};
        RenderUploadMode upload_mode{};
        std::vector<NodeState> nodes{};
        std::vector<uint32_t> slot_nodes{}; // the node whose tile is in each slot, or TERRAIN_NO_NODE
        std::vector<uint32_t> free_slots{};
        std::vector<uint32_t> loading{};    // nodes whose tiles are being copied in
        uint64_t last_seen{};
    };

    // A tile that is needed but isn't in the cache
    struct Request {
        float priority; // lower first
        uint32_t node;
    };

    // What the patches are being chosen for
    struct View {
        glm::vec3 camera{};              // in the terrain's space
        glm::vec3 world_camera{};
        std::array<glm::vec4, 5> planes{}; // of the camera's view, in the terrain's space: a point p is inside if dot(plane, (p, 1)) >= 0
        std::array<float, TERRAIN_MAX_LEVELS> ranges{};
        std::array<float, TERRAIN_MAX_LEVELS> morph_starts{};
    };

    ResourceManager& m_resource_manager;
    RenderBackend& m_render_backend;
    RenderObjectManager m_render_object_manager; // the terrains' materials
    const uint32_t m_slot_count;

    std::unordered_map<Entity, std::unique_ptr<Instance>> m_instances{};

    Settings m_settings{};
    Stats m_stats{};
    uint64_t m_frame{};

    bool m_frozen{false};
    glm::mat4 m_frozen_camera_matrix{1.0f}; // world matrix of the camera, while frozen
    glm::mat4 m_frozen_projection{1.0f};

    // (kept to save allocating them every frame)
    std::vector<TerrainPatch> m_patches{};
    std::vector<TerrainPatch> m_sorted_patches{};
    std::vector<float> m_patch_distances{};
    std::vector<uint32_t> m_patch_order{};
    std::vector<Request> m_requests{};

public:
    // slot_count: how many tiles each terrain's cache holds. A tile of 64 quads is 16.5 KiB
    TerrainSystem(World& world, ResourceManager& resource_manager, RenderBackend& render_backend, uint32_t slot_count = 3072);
    TerrainSystem(const TerrainSystem&) = delete;

    ~TerrainSystem() override;

    TerrainSystem& operator=(const TerrainSystem&) = delete;

    void onUpdate(FrameState& frame_state) override;

    Settings& getSettings() { return m_settings; }
    const Stats& getStats() const { return m_stats; }

    // Draws the system's debug window, for DebugUI::getWindowOpen()
    void renderDebugWindow(bool* open);

private:
    Instance* getInstance(Entity entity, Name terrain);
    void updateUploads(Instance& instance);
    bool selectNode(Instance& instance, const View& view, const TerrainNodeRef& node, const TerrainNodeRef& parent, bool visible);
    void addPatch(const Instance& instance, const View& view, const TerrainNodeRef& node, const TerrainNodeRef& parent, uint32_t quarter);
    void need(Instance& instance, const View& view, const TerrainNodeRef& node, bool visible);
    void startUploads(Instance& instance);
};

} // namespace gc
