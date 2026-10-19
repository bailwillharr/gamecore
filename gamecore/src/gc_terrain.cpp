#include "gamecore/gc_terrain.h"

#include <cmath>
#include <cstring>

#include <algorithm>
#include <array>
#include <limits>

#include <glm/geometric.hpp>

#include "gamecore/gc_assert.h"

#include "gc_collision_math.h"

namespace gc {

namespace {

// The part of a ray that is inside a box, clipped to [0, max_t]. False if there is none.
// inverse_direction is 1 / direction, which is infinite for the axes the ray doesn't move along.
bool rayBoxRange(const glm::vec3& origin, const glm::vec3& inverse_direction, const AABB& box, float max_t, float& t_enter, float& t_exit)
{
    float t_min = 0.0f;
    float t_max = max_t;
    for (int axis = 0; axis < 3; ++axis) {
        const float t1 = (box.min[axis] - origin[axis]) * inverse_direction[axis];
        const float t2 = (box.max[axis] - origin[axis]) * inverse_direction[axis];
        // written so that a NaN (zero times infinity, when the ray lies in one of the box's faces) leaves the range alone
        t_min = std::max(t_min, std::min(t1, t2));
        t_max = std::min(t_max, std::max(t1, t2));
    }
    t_enter = t_min;
    t_exit = t_max;
    return t_max >= t_min;
}

bool boxesOverlapXY(const AABB& a, const AABB& b) { return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y; }

bool hasChildren(const TerrainNode& node)
{
    return node.children[0] != TERRAIN_NO_NODE || node.children[1] != TERRAIN_NO_NODE || node.children[2] != TERRAIN_NO_NODE ||
           node.children[3] != TERRAIN_NO_NODE;
}

} // namespace

glm::vec3 TerrainSample::normal() const
{
    const float x = static_cast<float>(static_cast<int8_t>((bits >> 16) & 0xff)) / 127.0f;
    const float y = static_cast<float>(static_cast<int8_t>((bits >> 24) & 0xff)) / 127.0f;
    return glm::vec3{x, y, std::sqrt(std::max(0.0f, 1.0f - x * x - y * y))};
}

std::optional<ResourceTerrain> ResourceTerrain::create(std::span<const uint8_t> asset_data)
{
    if (asset_data.size() < sizeof(TerrainHeader)) {
        return {};
    }
    ResourceTerrain terrain{};
    std::memcpy(&terrain.header, asset_data.data(), sizeof(TerrainHeader));
    const TerrainHeader& h = terrain.header;
    if (h.version != TERRAIN_VERSION || h.tile_quads < 2 || h.tile_quads > 4096 || (h.tile_quads & (h.tile_quads - 1)) != 0 || h.level_count == 0 ||
        h.level_count > TERRAIN_MAX_LEVELS || h.root_count_x == 0 || h.root_count_y == 0 || h.root_count_x > 4096 || h.root_count_y > 4096 ||
        h.node_count < h.root_count_x * h.root_count_y || !(h.sample_spacing > 0.0f) || !std::isfinite(h.height_offset) || !std::isfinite(h.height_scale)) {
        return {};
    }
    const size_t tile_bytes = terrain.getTileSampleCount() * sizeof(uint32_t);
    const size_t nodes_bytes = static_cast<size_t>(h.node_count) * sizeof(TerrainNode);
    if (asset_data.size() != sizeof(TerrainHeader) + nodes_bytes + h.node_count * tile_bytes) {
        return {};
    }

    const uint8_t* const nodes_location = asset_data.data() + sizeof(TerrainHeader);
    const uint8_t* const samples_location = nodes_location + nodes_bytes;
    terrain.nodes = std::span(reinterpret_cast<const TerrainNode*>(nodes_location), h.node_count);
    terrain.samples = std::span(reinterpret_cast<const uint32_t*>(samples_location), h.node_count * terrain.getTileSampleCount());

    // The queries follow the children without checking them, so they are checked once here
    for (const TerrainNode& node : terrain.nodes) {
        for (const uint32_t child : node.children) {
            if (child != TERRAIN_NO_NODE && child >= h.node_count) {
                return {};
            }
        }
    }
    return terrain;
}

std::optional<ResourceTerrain> ResourceTerrain::create(const Content& content_manager, Name name)
{
    const auto asset = content_manager.findAsset(name);
    if (asset.type != gcpak::GcpakAssetType::TERRAIN) {
        return {};
    }
    return create(asset.data);
}

glm::vec2 ResourceTerrain::getSize() const
{
    const float root_size = getNodeSize(getTopLevel());
    return glm::vec2{root_size * static_cast<float>(header.root_count_x), root_size * static_cast<float>(header.root_count_y)};
}

glm::vec2 ResourceTerrain::getDataMin() const
{
    return glm::vec2{static_cast<float>(header.data_min_x), static_cast<float>(header.data_min_y)} * header.sample_spacing;
}

glm::vec2 ResourceTerrain::getDataMax() const
{
    // (the last sample is at max - 1)
    return glm::vec2{static_cast<float>(header.data_max_x - 1), static_cast<float>(header.data_max_y - 1)} * header.sample_spacing;
}

TerrainNodeRef ResourceTerrain::getRoot(uint32_t x, uint32_t y) const
{
    GC_ASSERT(x < header.root_count_x && y < header.root_count_y);
    return TerrainNodeRef{y * header.root_count_x + x, getTopLevel(), x, y};
}

TerrainNodeRef ResourceTerrain::getChild(const TerrainNodeRef& node, uint32_t child) const
{
    GC_ASSERT(node.level > 0 && child < 4);
    return TerrainNodeRef{nodes[node.index].children[child], node.level - 1, node.x * 2 + (child & 1), node.y * 2 + (child >> 1)};
}

AABB ResourceTerrain::getBounds() const
{
    const glm::vec2 size = getSize();
    return AABB{glm::vec3{0.0f, 0.0f, header.min_height}, glm::vec3{size.x, size.y, header.max_height}};
}

AABB ResourceTerrain::getNodeBounds(const TerrainNodeRef& node) const
{
    const float size = getNodeSize(node.level);
    const TerrainNode& n = nodes[node.index];
    const float x = static_cast<float>(node.x) * size;
    const float y = static_cast<float>(node.y) * size;
    return AABB{glm::vec3{x, y, n.min_height}, glm::vec3{x + size, y + size, n.max_height}};
}

std::optional<TerrainNodeRef> ResourceTerrain::findNode(float x, float y) const
{
    const glm::vec2 size = getSize();
    if (!(x >= 0.0f && y >= 0.0f && x <= size.x && y <= size.y)) {
        return {};
    }
    const float root_size = getNodeSize(getTopLevel());
    const uint32_t root_x = std::min(static_cast<uint32_t>(x / root_size), header.root_count_x - 1);
    const uint32_t root_y = std::min(static_cast<uint32_t>(y / root_size), header.root_count_y - 1);
    TerrainNodeRef node = getRoot(root_x, root_y);
    while (node.level > 0) {
        const float half = getNodeSize(node.level) * 0.5f;
        const float node_x = static_cast<float>(node.x) * half * 2.0f;
        const float node_y = static_cast<float>(node.y) * half * 2.0f;
        const uint32_t child = ((x >= node_x + half) ? 1u : 0u) | ((y >= node_y + half) ? 2u : 0u);
        const TerrainNodeRef child_ref = getChild(node, child);
        if (child_ref.index == TERRAIN_NO_NODE) {
            break;
        }
        node = child_ref;
    }
    return node;
}

std::optional<float> ResourceTerrain::getHeight(float x, float y) const
{
    const std::optional<TerrainNodeRef> node = findNode(x, y);
    if (!node) {
        return {};
    }
    const float cell_size = getCellSize(node->level);
    const float node_size = getNodeSize(node->level);
    const float u = (x - static_cast<float>(node->x) * node_size) / cell_size;
    const float v = (y - static_cast<float>(node->y) * node_size) / cell_size;
    const uint32_t last_cell = header.tile_quads - 1;
    const uint32_t i = std::min(static_cast<uint32_t>(std::max(u, 0.0f)), last_cell);
    const uint32_t j = std::min(static_cast<uint32_t>(std::max(v, 0.0f)), last_cell);
    const float fx = std::clamp(u - static_cast<float>(i), 0.0f, 1.0f);
    const float fy = std::clamp(v - static_cast<float>(j), 0.0f, 1.0f);

    const std::span<const uint32_t> tile = getTile(node->index);
    const uint32_t side = getTileSide();
    const float h00 = decodeHeight(tile[j * side + i]);
    const float h10 = decodeHeight(tile[j * side + i + 1]);
    const float h01 = decodeHeight(tile[(j + 1) * side + i]);
    const float h11 = decodeHeight(tile[(j + 1) * side + i + 1]);
    // the cell's two triangles meet along the diagonal from (0, 0) to (1, 1)
    if (fx >= fy) {
        return h00 + fx * (h10 - h00) + fy * (h11 - h10);
    }
    return h00 + fy * (h01 - h00) + fx * (h11 - h01);
}

bool ResourceTerrain::raycastCells(const TerrainNodeRef& node, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const glm::vec3& origin,
                                   const glm::vec3& direction, float t_enter, float t_exit, float& t_out, glm::vec3& normal_out) const
{
    const float cell_size = getCellSize(node.level);
    const float node_size = getNodeSize(node.level);
    const float node_x = static_cast<float>(node.x) * node_size;
    const float node_y = static_cast<float>(node.y) * node_size;
    const std::span<const uint32_t> tile = getTile(node.index);
    const uint32_t side = getTileSide();

    // The cell that the ray starts in
    const glm::vec3 start = origin + direction * t_enter;
    int32_t i = std::clamp(static_cast<int32_t>(std::floor((start.x - node_x) / cell_size)), static_cast<int32_t>(x0), static_cast<int32_t>(x1) - 1);
    int32_t j = std::clamp(static_cast<int32_t>(std::floor((start.y - node_y) / cell_size)), static_cast<int32_t>(y0), static_cast<int32_t>(y1) - 1);

    // Then the cells that it crosses, in order (Amanatides and Woo). t_next_* is how far along the ray the next cell boundary
    // across x (or y) is, and t_delta_* is how far it is between boundaries.
    constexpr float NEVER = std::numeric_limits<float>::infinity();
    const int32_t step_x = (direction.x > 0.0f) ? 1 : ((direction.x < 0.0f) ? -1 : 0);
    const int32_t step_y = (direction.y > 0.0f) ? 1 : ((direction.y < 0.0f) ? -1 : 0);
    float t_next_x = NEVER;
    float t_next_y = NEVER;
    float t_delta_x = NEVER;
    float t_delta_y = NEVER;
    if (step_x != 0) {
        const float boundary = node_x + static_cast<float>(i + (step_x > 0 ? 1 : 0)) * cell_size;
        t_next_x = (boundary - origin.x) / direction.x;
        t_delta_x = cell_size / std::abs(direction.x);
    }
    if (step_y != 0) {
        const float boundary = node_y + static_cast<float>(j + (step_y > 0 ? 1 : 0)) * cell_size;
        t_next_y = (boundary - origin.y) / direction.y;
        t_delta_y = cell_size / std::abs(direction.y);
    }

    float t_cell_enter = t_enter;
    for (;;) {
        const float t_cell_exit = std::min({t_next_x, t_next_y, t_exit});

        const uint32_t s00 = tile[static_cast<uint32_t>(j) * side + static_cast<uint32_t>(i)];
        const uint32_t s10 = tile[static_cast<uint32_t>(j) * side + static_cast<uint32_t>(i) + 1];
        const uint32_t s01 = tile[static_cast<uint32_t>(j + 1) * side + static_cast<uint32_t>(i)];
        const uint32_t s11 = tile[static_cast<uint32_t>(j + 1) * side + static_cast<uint32_t>(i) + 1];
        const float h00 = decodeHeight(s00);
        const float h10 = decodeHeight(s10);
        const float h01 = decodeHeight(s01);
        const float h11 = decodeHeight(s11);

        // Only look at the triangles if the ray is at their heights while it is over the cell
        const float z_enter = origin.z + direction.z * t_cell_enter;
        const float z_exit = origin.z + direction.z * t_cell_exit;
        const float cell_min = std::min({h00, h10, h01, h11});
        const float cell_max = std::max({h00, h10, h01, h11});
        if (std::min(z_enter, z_exit) <= cell_max && std::max(z_enter, z_exit) >= cell_min) {
            const float x = node_x + static_cast<float>(i) * cell_size;
            const float y = node_y + static_cast<float>(j) * cell_size;
            const glm::vec3 p00{x, y, h00};
            const glm::vec3 p10{x + cell_size, y, h10};
            const glm::vec3 p01{x, y + cell_size, h01};
            const glm::vec3 p11{x + cell_size, y + cell_size, h11};
            float nearest = t_exit;
            bool hit = false;
            float t{};
            if (rayHitsTriangle(origin, direction, p00, p10, p11, t) && t <= nearest) {
                nearest = t;
                normal_out = glm::cross(p10 - p00, p11 - p00);
                hit = true;
            }
            if (rayHitsTriangle(origin, direction, p00, p11, p01, t) && t <= nearest) {
                nearest = t;
                normal_out = glm::cross(p11 - p00, p01 - p00);
                hit = true;
            }
            // Nothing in a later cell can be nearer than a hit in this one
            if (hit) {
                t_out = nearest;
                return true;
            }
        }

        if (t_cell_exit >= t_exit) {
            return false;
        }
        if (t_next_x < t_next_y) {
            i += step_x;
            t_cell_enter = t_next_x;
            t_next_x += t_delta_x;
        }
        else {
            j += step_y;
            t_cell_enter = t_next_y;
            t_next_y += t_delta_y;
        }
        if (i < static_cast<int32_t>(x0) || i >= static_cast<int32_t>(x1) || j < static_cast<int32_t>(y0) || j >= static_cast<int32_t>(y1)) {
            return false;
        }
    }
}

bool ResourceTerrain::raycast(const glm::vec3& origin, const glm::vec3& direction, float max_t, float& t_out, glm::vec3& normal_out) const
{
    if (!(glm::dot(direction, direction) > 0.0f)) {
        return false;
    }
    const glm::vec3 inverse_direction = 1.0f / direction;
    const uint32_t half_quads = header.tile_quads / 2;

    // What is left to look at, nearest last. A node is looked into, or, where a node has no child, the cells of that quarter of
    // its tile are walked through. A quarter of -1 is the whole tile, for a node with no children at all.
    struct Item {
        TerrainNodeRef node;
        int32_t quarter; // -2: the node itself
        float t_enter;
    };
    constexpr int32_t NODE = -2;
    constexpr int32_t WHOLE_TILE = -1;
    std::vector<Item> stack{};
    stack.reserve(64);

    float nearest = max_t;
    bool hit = false;

    std::vector<Item> found{};
    found.reserve(4);
    const auto push_nearest_last = [&]() {
        std::sort(found.begin(), found.end(), [](const Item& a, const Item& b) { return a.t_enter > b.t_enter; });
        stack.insert(stack.end(), found.begin(), found.end());
        found.clear();
    };

    for (uint32_t y = 0; y < header.root_count_y; ++y) {
        for (uint32_t x = 0; x < header.root_count_x; ++x) {
            const TerrainNodeRef root = getRoot(x, y);
            float t_enter{}, t_exit{};
            if (rayBoxRange(origin, inverse_direction, getNodeBounds(root), nearest, t_enter, t_exit)) {
                found.push_back(Item{root, NODE, t_enter});
            }
        }
    }
    push_nearest_last();

    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        if (item.t_enter > nearest) {
            continue; // something nearer has been hit since
        }
        const TerrainNode& node = nodes[item.node.index];
        const AABB node_box = getNodeBounds(item.node);

        if (item.quarter == NODE) {
            if (!hasChildren(node)) {
                found.push_back(Item{item.node, WHOLE_TILE, item.t_enter});
            }
            else {
                for (uint32_t child = 0; child < 4; ++child) {
                    float t_enter{}, t_exit{};
                    if (node.children[child] != TERRAIN_NO_NODE) {
                        const TerrainNodeRef child_ref = getChild(item.node, child);
                        if (rayBoxRange(origin, inverse_direction, getNodeBounds(child_ref), nearest, t_enter, t_exit)) {
                            found.push_back(Item{child_ref, NODE, t_enter});
                        }
                    }
                    else {
                        // this node's own cells, under the child that isn't there
                        AABB quarter_box = node_box;
                        const glm::vec3 half = (node_box.max - node_box.min) * 0.5f;
                        quarter_box.min.x += (child & 1) ? half.x : 0.0f;
                        quarter_box.max.x -= (child & 1) ? 0.0f : half.x;
                        quarter_box.min.y += (child & 2) ? half.y : 0.0f;
                        quarter_box.max.y -= (child & 2) ? 0.0f : half.y;
                        if (rayBoxRange(origin, inverse_direction, quarter_box, nearest, t_enter, t_exit)) {
                            found.push_back(Item{item.node, static_cast<int32_t>(child), t_enter});
                        }
                    }
                }
            }
            push_nearest_last();
            continue;
        }

        // Walk through cells
        uint32_t x0 = 0, y0 = 0, x1 = header.tile_quads, y1 = header.tile_quads;
        AABB box = node_box;
        if (item.quarter != WHOLE_TILE) {
            const uint32_t quarter = static_cast<uint32_t>(item.quarter);
            x0 = (quarter & 1) ? half_quads : 0;
            y0 = (quarter & 2) ? half_quads : 0;
            x1 = x0 + half_quads;
            y1 = y0 + half_quads;
            const float cell_size = getCellSize(item.node.level);
            box.min.x += static_cast<float>(x0) * cell_size;
            box.min.y += static_cast<float>(y0) * cell_size;
            box.max.x = box.min.x + static_cast<float>(half_quads) * cell_size;
            box.max.y = box.min.y + static_cast<float>(half_quads) * cell_size;
        }
        float t_enter{}, t_exit{};
        if (!rayBoxRange(origin, inverse_direction, box, nearest, t_enter, t_exit)) {
            continue;
        }
        float t{};
        glm::vec3 normal{};
        if (raycastCells(item.node, x0, y0, x1, y1, origin, direction, t_enter, t_exit, t, normal) && t <= nearest) {
            nearest = t;
            normal_out = normal;
            hit = true;
        }
    }

    if (hit) {
        t_out = nearest;
    }
    return hit;
}

void ResourceTerrain::appendCellTriangles(const TerrainNodeRef& node, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const AABB& box,
                                          std::vector<glm::vec3>& corners_out) const
{
    const float cell_size = getCellSize(node.level);
    const float node_size = getNodeSize(node.level);
    const float node_x = static_cast<float>(node.x) * node_size;
    const float node_y = static_cast<float>(node.y) * node_size;

    // the cells under the box
    const auto first_cell = [&](float position, float node_position, uint32_t lowest) {
        const float cell = std::floor((position - node_position) / cell_size);
        return (cell <= static_cast<float>(lowest)) ? lowest : static_cast<uint32_t>(cell);
    };
    const auto end_cell = [&](float position, float node_position, uint32_t highest) {
        const float cell = std::floor((position - node_position) / cell_size) + 1.0f;
        return (cell >= static_cast<float>(highest)) ? highest : static_cast<uint32_t>(std::max(cell, 0.0f));
    };
    const uint32_t i0 = first_cell(box.min.x, node_x, x0);
    const uint32_t i1 = end_cell(box.max.x, node_x, x1);
    const uint32_t j0 = first_cell(box.min.y, node_y, y0);
    const uint32_t j1 = end_cell(box.max.y, node_y, y1);

    const std::span<const uint32_t> tile = getTile(node.index);
    const uint32_t side = getTileSide();
    for (uint32_t j = j0; j < j1; ++j) {
        for (uint32_t i = i0; i < i1; ++i) {
            const float h00 = decodeHeight(tile[j * side + i]);
            const float h10 = decodeHeight(tile[j * side + i + 1]);
            const float h01 = decodeHeight(tile[(j + 1) * side + i]);
            const float h11 = decodeHeight(tile[(j + 1) * side + i + 1]);
            if (std::max({h00, h10, h01, h11}) < box.min.z || std::min({h00, h10, h01, h11}) > box.max.z) {
                continue;
            }
            const float x = node_x + static_cast<float>(i) * cell_size;
            const float y = node_y + static_cast<float>(j) * cell_size;
            const glm::vec3 p00{x, y, h00};
            const glm::vec3 p10{x + cell_size, y, h10};
            const glm::vec3 p01{x, y + cell_size, h01};
            const glm::vec3 p11{x + cell_size, y + cell_size, h11};
            corners_out.insert(corners_out.end(), {p00, p10, p11, p00, p11, p01});
        }
    }
}

void ResourceTerrain::getTriangles(const AABB& box, std::vector<glm::vec3>& corners_out) const
{
    const uint32_t half_quads = header.tile_quads / 2;
    const auto overlaps = [&](const AABB& node_box) {
        return boxesOverlapXY(node_box, box) && node_box.min.z <= box.max.z && node_box.max.z >= box.min.z;
    };

    std::vector<TerrainNodeRef> stack{};
    stack.reserve(32);
    for (uint32_t y = 0; y < header.root_count_y; ++y) {
        for (uint32_t x = 0; x < header.root_count_x; ++x) {
            if (overlaps(getNodeBounds(getRoot(x, y)))) {
                stack.push_back(getRoot(x, y));
            }
        }
    }

    while (!stack.empty()) {
        const TerrainNodeRef node_ref = stack.back();
        stack.pop_back();
        const TerrainNode& node = nodes[node_ref.index];
        if (!hasChildren(node)) {
            appendCellTriangles(node_ref, 0, 0, header.tile_quads, header.tile_quads, box, corners_out);
            continue;
        }
        for (uint32_t child = 0; child < 4; ++child) {
            if (node.children[child] != TERRAIN_NO_NODE) {
                const TerrainNodeRef child_ref = getChild(node_ref, child);
                if (overlaps(getNodeBounds(child_ref))) {
                    stack.push_back(child_ref);
                }
            }
            else {
                const uint32_t x0 = (child & 1) ? half_quads : 0;
                const uint32_t y0 = (child & 2) ? half_quads : 0;
                appendCellTriangles(node_ref, x0, y0, x0 + half_quads, y0 + half_quads, box, corners_out);
            }
        }
    }
}

} // namespace gc
