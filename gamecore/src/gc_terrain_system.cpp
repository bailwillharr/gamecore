#include "gamecore/gc_terrain_system.h"

#include <cmath>

#include <algorithm>
#include <limits>
#include <numeric>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <imgui.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_camera_component.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_render_texture_manager.h"
#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_terrain_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_window.h"
#include "gamecore/gc_world.h"

namespace gc {

namespace {

// A patch starts turning into its parent at this fraction of its level's range
constexpr float MORPH_START = 0.7f;
// Children are asked for when their parent is within this much more than the range at which it splits into them, so that they
// have usually arrived by the time they are drawn
constexpr float PREFETCH = 1.3f;
// At most this many tiles of a terrain are copied in at once
constexpr size_t MAX_LOADING = 512;
// A patch at a distance that turns into nothing (the top level's) never turns
constexpr float NEVER = 1.0e30f;

float distanceSquaredToBox(const AABB& box, const glm::vec3& point)
{
    const glm::vec3 nearest = glm::clamp(point, box.min, box.max);
    const glm::vec3 d = point - nearest;
    return glm::dot(d, d);
}

bool boxInSphere(const AABB& box, const glm::vec3& centre, float radius) { return distanceSquaredToBox(box, centre) <= radius * radius; }

bool boxInView(const AABB& box, const std::array<glm::vec4, 5>& planes)
{
    for (const glm::vec4& plane : planes) {
        // the corner furthest along the plane's normal
        const glm::vec3 corner{plane.x > 0.0f ? box.max.x : box.min.x, plane.y > 0.0f ? box.max.y : box.min.y, plane.z > 0.0f ? box.max.z : box.min.z};
        if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

glm::vec4 getRow(const glm::mat4& m, int row) { return glm::vec4{m[0][row], m[1][row], m[2][row], m[3][row]}; }

} // namespace

TerrainSystem::TerrainSystem(World& world, ResourceManager& resource_manager, RenderBackend& render_backend, uint32_t slot_count)
    : System(world),
      m_resource_manager(resource_manager),
      m_render_backend(render_backend),
      m_render_object_manager(resource_manager, render_backend),
      m_slot_count(slot_count)
{
}

TerrainSystem::~TerrainSystem() = default;

void TerrainSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    ++m_frame;
    const uint64_t total_uploads = m_stats.total_uploads;
    const uint64_t total_upload_bytes = m_stats.total_upload_bytes;
    m_stats = Stats{};
    m_stats.total_uploads = total_uploads;
    m_stats.total_upload_bytes = total_upload_bytes;

    if (!m_world.isComponentRegistered(TerrainComponent::NAME) || !frame_state.window_state) {
        return;
    }

    // The camera, as the CameraSystem sees it
    bool has_camera = false;
    glm::mat4 camera_matrix{1.0f};
    glm::mat4 projection{1.0f};
    const glm::ivec2 window_size = frame_state.window_state->getWindowSize();
    const float aspect_ratio = static_cast<float>(window_size.x) / static_cast<float>(std::max(window_size.y, 1));
    m_world.forEach<TransformComponent, CameraComponent>([&](Entity, const TransformComponent& t, const CameraComponent& c) {
        if (c.getActive()) {
            camera_matrix = t.getWorldMatrix();
            projection = glm::infinitePerspectiveRH_NO(c.getFOV(), aspect_ratio, c.getNearPlane());
            projection[2][2] = 0.0f;
            projection[3][2] = c.getNearPlane();
            projection[1][1] *= -1.0f;
            has_camera = true;
        }
    });
    if (!has_camera) {
        return;
    }
    if (m_settings.freeze && !m_frozen) {
        m_frozen_camera_matrix = camera_matrix;
        m_frozen_projection = projection;
    }
    m_frozen = m_settings.freeze;
    if (m_frozen) {
        camera_matrix = m_frozen_camera_matrix;
        projection = m_frozen_projection;
    }
    const glm::mat4 view_projection = projection * glm::inverse(camera_matrix);

    m_world.forEach<TransformComponent, TerrainComponent>([&](Entity entity, const TransformComponent& t, const TerrainComponent& c) {
        Instance* const instance = getInstance(entity, c.m_terrain);
        if (!instance) {
            return;
        }
        instance->last_seen = m_frame;
        ++m_stats.terrains;
        updateUploads(*instance);

        const ResourceTerrain& terrain = *instance->resource;
        const glm::mat4 world_matrix = t.getWorldMatrix();

        View view{};
        view.world_camera = glm::vec3(camera_matrix[3]);
        view.camera = glm::vec3(glm::inverse(world_matrix) * glm::vec4(view.world_camera, 1.0f));
        const glm::mat4 local_view_projection = view_projection * world_matrix;
        const glm::vec4 row0 = getRow(local_view_projection, 0);
        const glm::vec4 row1 = getRow(local_view_projection, 1);
        const glm::vec4 row2 = getRow(local_view_projection, 2);
        const glm::vec4 row3 = getRow(local_view_projection, 3);
        // left, right, bottom, top, and near (the depth buffer is reversed, so the near plane is at 1). There is no far plane
        view.planes = {row3 + row0, row3 - row0, row3 + row1, row3 - row1, row3 - row2};

        // Each level's range is twice the one below's. A range has to be a few tiles across, for neighbouring patches to be no
        // more than a level apart (and for a patch to have turned into its parent before its neighbour is drawn at the parent's level)
        const float detail_distance = std::max(c.m_detail_distance * m_settings.detail_scale, 3.0f * terrain.getNodeSize(0));
        for (uint32_t level = 0; level < terrain.header.level_count; ++level) {
            const bool top = (level == terrain.getTopLevel());
            view.ranges[level] = top ? NEVER : detail_distance * static_cast<float>(1u << level);
            view.morph_starts[level] = top ? NEVER : view.ranges[level] * MORPH_START;
        }

        // (even if the terrain isn't drawn, so that its tiles are there when it is)
        m_patches.clear();
        m_requests.clear();
        for (uint32_t y = 0; y < terrain.header.root_count_y; ++y) {
            for (uint32_t x = 0; x < terrain.header.root_count_x; ++x) {
                const TerrainNodeRef root = terrain.getRoot(x, y);
                selectNode(*instance, view, root, root, true);
            }
        }
        startUploads(*instance);

        m_stats.slots += instance->render->getSlotCount();
        m_stats.resident += instance->render->getSlotCount() - static_cast<uint32_t>(instance->free_slots.size()) -
                            static_cast<uint32_t>(instance->loading.size());
        m_stats.loading += static_cast<uint32_t>(instance->loading.size());

        if (!c.m_visible || m_patches.empty() || !instance->render->isReady()) {
            return;
        }
        RenderMaterial* const material = m_render_object_manager.getRenderMaterial(c.m_material);
        material->setLastUsedFrame(frame_state.frame_count);

        // Nearest first, so that the depth test throws away more of what is behind
        m_patch_distances.resize(m_patches.size());
        for (size_t i = 0; i < m_patches.size(); ++i) {
            const TerrainPatch& patch = m_patches[i];
            const float half = patch.spacing * static_cast<float>(terrain.header.tile_quads / 4);
            const glm::vec2 centre{patch.x + half, patch.y + half};
            m_patch_distances[i] = glm::dot(centre - glm::vec2(view.camera), centre - glm::vec2(view.camera));
        }
        m_patch_order.resize(m_patches.size());
        std::iota(m_patch_order.begin(), m_patch_order.end(), 0u);
        std::sort(m_patch_order.begin(), m_patch_order.end(), [&](uint32_t a, uint32_t b) { return m_patch_distances[a] < m_patch_distances[b]; });
        m_sorted_patches.clear();
        for (const uint32_t i : m_patch_order) {
            m_sorted_patches.push_back(m_patches[i]);
        }

        frame_state.draw_data.drawTerrain(instance->render.get(), material, world_matrix, terrain.header.height_offset, terrain.header.height_scale,
                                          m_sorted_patches);
        const uint64_t quads = terrain.header.tile_quads / 2;
        m_stats.patches += static_cast<uint32_t>(m_sorted_patches.size());
        m_stats.triangles += m_sorted_patches.size() * quads * quads * 2;
    });

    // Forget the terrains of entities that have gone
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        if (it->second->last_seen != m_frame) {
            it = m_instances.erase(it);
        }
        else {
            ++it;
        }
    }

    constexpr uint64_t INACTIVE_OBJECT_LIFETIME_FRAMES = 10;
    if (frame_state.frame_count > INACTIVE_OBJECT_LIFETIME_FRAMES) {
        m_render_object_manager.deleteUnusedObjects(frame_state.frame_count - INACTIVE_OBJECT_LIFETIME_FRAMES);
    }
}

TerrainSystem::Instance* TerrainSystem::getInstance(Entity entity, Name terrain)
{
    auto it = m_instances.find(entity);
    if (it != m_instances.end() && it->second->terrain != terrain) {
        m_instances.erase(it);
        it = m_instances.end();
    }
    if (it == m_instances.end()) {
        auto instance = std::make_unique<Instance>();
        instance->terrain = terrain;
        instance->resource = terrain.empty() ? nullptr : m_resource_manager.get<ResourceTerrain>(terrain);
        if (instance->resource) {
            const ResourceTerrain& resource = *instance->resource;
            instance->render = std::make_unique<RenderTerrain>(m_render_backend.createTerrain(resource.header.tile_quads, m_slot_count));
            instance->upload_mode = getRenderUploadMode(m_resource_manager, resource.samples);
            instance->nodes.resize(resource.header.node_count);
            const uint32_t slot_count = instance->render->getSlotCount();
            instance->slot_nodes.assign(slot_count, TERRAIN_NO_NODE);
            instance->free_slots.resize(slot_count);
            // (taken from the back, so that the first slots are used first)
            std::iota(instance->free_slots.rbegin(), instance->free_slots.rend(), 0u);
            GC_INFO("Drawing the terrain {}: {} nodes in {} levels, {:.1f} x {:.1f} km, with a cache of {} tiles", terrain, resource.header.node_count,
                    resource.header.level_count, resource.getSize().x / 1000.0f, resource.getSize().y / 1000.0f, slot_count);
        }
        else if (!terrain.empty()) {
            GC_ERROR("Could not load the terrain {}", terrain);
        }
        it = m_instances.emplace(entity, std::move(instance)).first;
    }
    return it->second->resource ? it->second.get() : nullptr;
}

void TerrainSystem::updateUploads(Instance& instance)
{
    std::erase_if(instance.loading, [&](uint32_t node) {
        NodeState& state = instance.nodes[node];
        if (state.state != TileState::LOADING) {
            return true; // (evicted while loading)
        }
        if (instance.render->isTileUploaded(state.slot)) {
            state.state = TileState::RESIDENT;
            return true;
        }
        return false;
    });
}

void TerrainSystem::need(Instance& instance, const View& view, const TerrainNodeRef& node, bool visible)
{
    NodeState& state = instance.nodes[node.index];
    if (state.last_needed != m_frame) {
        state.last_needed = m_frame;
        ++m_stats.needed;
        if (state.state != TileState::RESIDENT) {
            ++m_stats.missing;
        }
    }
    if (state.state == TileState::NONE) {
        // Coarse tiles that are near and can be seen first. (Nothing finer can be drawn without them)
        const float size = instance.resource->getNodeSize(node.level);
        const float distance = std::sqrt(distanceSquaredToBox(instance.resource->getNodeBounds(node), view.camera));
        const float priority = distance / size + (visible ? 0.0f : 4.0f);
        m_requests.push_back(Request{priority, node.index});
    }
}

bool TerrainSystem::selectNode(Instance& instance, const View& view, const TerrainNodeRef& node, const TerrainNodeRef& parent, bool visible)
{
    const ResourceTerrain& terrain = *instance.resource;
    const AABB box = terrain.getNodeBounds(node);
    if (!boxInSphere(box, view.camera, view.ranges[node.level])) {
        return false; // the parent covers it
    }
    need(instance, view, node, visible);
    if (instance.nodes[node.index].state != TileState::RESIDENT) {
        return false; // not here yet, so the parent covers it
    }
    visible = visible && boxInView(box, view.planes);

    const bool split = node.level > 0 && boxInSphere(box, view.camera, view.ranges[node.level - 1]);
    const bool prefetch = !split && node.level > 0 && boxInSphere(box, view.camera, view.ranges[node.level - 1] * PREFETCH);
    for (uint32_t quarter = 0; quarter < 4; ++quarter) {
        bool covered = false;
        if (split || prefetch) {
            const TerrainNodeRef child = terrain.getChild(node, quarter);
            if (child.index != TERRAIN_NO_NODE) {
                if (split) {
                    covered = selectNode(instance, view, child, node, visible);
                }
                else {
                    need(instance, view, child, visible);
                }
            }
        }
        if (!covered && visible) {
            addPatch(instance, view, node, parent, quarter);
        }
    }
    return true;
}

void TerrainSystem::addPatch(const Instance& instance, const View& view, const TerrainNodeRef& node, const TerrainNodeRef& parent, uint32_t quarter)
{
    const ResourceTerrain& terrain = *instance.resource;
    const uint32_t half = terrain.header.tile_quads / 2;
    const float cell_size = terrain.getCellSize(node.level);

    // only if this quarter can be seen
    AABB box = terrain.getNodeBounds(node);
    box.min.x += (quarter & 1) ? static_cast<float>(half) * cell_size : 0.0f;
    box.min.y += (quarter & 2) ? static_cast<float>(half) * cell_size : 0.0f;
    box.max.x = box.min.x + static_cast<float>(half) * cell_size;
    box.max.y = box.min.y + static_cast<float>(half) * cell_size;
    if (!boxInView(box, view.planes)) {
        return;
    }

    const uint32_t origin_x = (quarter & 1) ? half : 0;
    const uint32_t origin_y = (quarter & 2) ? half : 0;
    const bool top = (node.level == terrain.getTopLevel());

    TerrainPatch patch{};
    patch.tile = instance.render->getTileOffset(instance.nodes[node.index].slot);
    patch.parent_tile = instance.render->getTileOffset(instance.nodes[parent.index].slot);
    patch.origin = origin_x | (origin_y << 12) | (node.level << 24) | (m_settings.show_levels ? (1u << 28) : 0u);
    // where this node's tile is in its parent's
    patch.parent_origin = top ? 0 : (((node.x & 1) ? half : 0) | (((node.y & 1) ? half : 0) << 16));
    patch.x = box.min.x;
    patch.y = box.min.y;
    patch.spacing = cell_size;
    patch.skirt_depth = terrain.nodes[node.index].skirt_depth;
    patch.morph_start = view.morph_starts[node.level];
    patch.morph_end = view.ranges[node.level];
    m_patches.push_back(patch);
    ++m_stats.patches_per_level[node.level];
}

void TerrainSystem::startUploads(Instance& instance)
{
    if (m_requests.empty()) {
        return;
    }
    std::sort(m_requests.begin(), m_requests.end(), [](const Request& a, const Request& b) { return a.priority < b.priority; });

    // Slots that can be taken back: their tiles weren't needed this frame. The ones needed longest ago go first, and the most
    // detailed of those (whose parents are more likely to be needed again)
    std::vector<uint32_t> evictable{};
    bool evictable_found = false;
    size_t next_evictable = 0;

    const ResourceTerrain& terrain = *instance.resource;
    uint32_t started = 0;
    for (const Request& request : m_requests) {
        if (started >= m_settings.uploads_per_frame || instance.loading.size() >= MAX_LOADING) {
            break;
        }
        NodeState& state = instance.nodes[request.node];
        if (state.state != TileState::NONE) {
            continue; // (asked for twice)
        }

        // Slots that have never had a tile first. (Slots aren't freed: once the cache is full, it stays full)
        uint32_t slot = TERRAIN_NO_NODE;
        if (!instance.free_slots.empty()) {
            slot = instance.free_slots.back();
            instance.free_slots.pop_back();
        }
        else {
            if (!evictable_found) {
                evictable_found = true;
                for (uint32_t s = 0; s < instance.slot_nodes.size(); ++s) {
                    const uint32_t node = instance.slot_nodes[s];
                    if (node != TERRAIN_NO_NODE && instance.nodes[node].state == TileState::RESIDENT && instance.nodes[node].last_needed != m_frame) {
                        evictable.push_back(s);
                    }
                }
                std::sort(evictable.begin(), evictable.end(), [&](uint32_t a, uint32_t b) {
                    const NodeState& na = instance.nodes[instance.slot_nodes[a]];
                    const NodeState& nb = instance.nodes[instance.slot_nodes[b]];
                    return na.last_needed < nb.last_needed;
                });
            }
            while (next_evictable < evictable.size()) {
                const uint32_t candidate = evictable[next_evictable++];
                if (instance.render->isSlotFree(candidate)) {
                    slot = candidate;
                    break;
                }
            }
            if (slot == TERRAIN_NO_NODE) {
                break; // the cache is full of tiles that are needed, or in use
            }
            NodeState& evicted = instance.nodes[instance.slot_nodes[slot]];
            evicted.state = TileState::NONE;
            evicted.slot = TERRAIN_NO_NODE;
            instance.render->evictTile(slot);
            ++m_stats.evictions;
        }

        const std::span<const uint32_t> tile = terrain.getTile(request.node);
        instance.render->uploadTile(slot, tile, instance.upload_mode);
        instance.slot_nodes[slot] = request.node;
        state.slot = slot;
        state.state = TileState::LOADING;
        instance.loading.push_back(request.node);
        ++started;
        ++m_stats.total_uploads;
        m_stats.total_upload_bytes += tile.size_bytes();
    }
    m_stats.uploads_started += started;
}

void TerrainSystem::renderDebugWindow(bool* open)
{
    if (ImGui::Begin("Terrain", open)) {
        const Stats& s = m_stats;
        ImGui::Text("%u terrain%s", s.terrains, s.terrains == 1 ? "" : "s");
        ImGui::Text("Patches drawn: %u (%.2f M triangles)", s.patches, static_cast<double>(s.triangles) / 1.0e6);
        if (ImGui::BeginTable("levels", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Level");
            ImGui::TableSetupColumn("Patches");
            ImGui::TableHeadersRow();
            for (uint32_t level = 0; level < s.patches_per_level.size(); ++level) {
                if (s.patches_per_level[level] == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", level);
                ImGui::TableNextColumn();
                ImGui::Text("%u", s.patches_per_level[level]);
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::Text("Tile cache: %u of %u slots used, %u loading", s.resident + s.loading, s.slots, s.loading);
        ImGui::Text("Tiles needed: %u, %u of them not there yet", s.needed, s.missing);
        ImGui::Text("This frame: %u uploads started, %u tiles evicted", s.uploads_started, s.evictions);
        ImGui::Text("Since the start: %llu tiles (%s) uploaded", static_cast<unsigned long long>(s.total_uploads),
                    bytesToHumanReadable(s.total_upload_bytes).c_str());
        ImGui::Separator();
        ImGui::SliderFloat("Detail", &m_settings.detail_scale, 0.25f, 4.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Colour the levels of detail", &m_settings.show_levels);
        ImGui::Checkbox("Freeze (keep the patches chosen for the camera where it is now)", &m_settings.freeze);
        int uploads = static_cast<int>(m_settings.uploads_per_frame);
        if (ImGui::SliderInt("Uploads per frame", &uploads, 1, 256)) {
            m_settings.uploads_per_frame = static_cast<uint32_t>(uploads);
        }
    }
    ImGui::End();
}

} // namespace gc
