#pragma once

// Terrain: a height field that is too big to draw at full detail, or to keep in GPU memory, all at once.
//
// A TERRAIN asset (see gcpak.h) is made beforehand from a grid of heights (gcpak_editor --bake-terrain, which uses
// makeTerrainData() in gc_terrain_builder.h). The grid is cut into square tiles of the same number of samples, at several levels
// of detail. Level 0 has every sample. Each level above it has every other sample of the one below (smoothed a little first), so a
// tile at level l covers 2^l times the ground that a tile at level 0 does. The tiles form a quadtree: a tile's four children
// are the tiles at the level below that cover the same ground. Each node of the tree knows the lowest and highest heights under
// it, so that it can be culled and so that collision queries can skip it.
//
// The TerrainSystem draws a terrain by picking nodes from the tree: detailed ones near the camera and coarse ones far away
// (see gc_terrain_system.h). Only the tiles of the nodes it picks are in GPU memory, so the asset itself can be any size: the
// tiles are read from the mapped .gcpak file as they are needed.
//
// The CollisionSystem collides with a terrain using the same tiles: the most detailed tile at each point, which is what is drawn
// near the camera. So what is seen is what is walked on. Nothing has to be built when a terrain is loaded.
//
// A tree doesn't need to have every tile at every level. Where the source data had no heights (around the edges of a scan,
// say), only the coarse levels are stored, and the holes are filled with a smooth surface. The tile of a node always exists if
// the node does, and so do its ancestors.
//
// The terrain's space: x and y along the grid (+x along a row, +y from row to row), z up, in metres. Sample (i, j) of level 0 is
// at (i * spacing, j * spacing). Each grid cell is two triangles, split along the diagonal from (i, j) to (i + 1, j + 1).

#include <cstdint>

#include <optional>
#include <span>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "gamecore/gc_collision_bvh.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_name.h"

namespace gc {

constexpr uint32_t TERRAIN_VERSION = 1;
constexpr uint32_t TERRAIN_NO_NODE = 0xffffffff;
constexpr uint32_t TERRAIN_MAX_LEVELS = 16;

// The start of a TERRAIN asset
struct TerrainHeader {
    uint32_t version;
    uint32_t tile_quads;   // the number of grid cells along a tile's side. A tile has tile_quads + 1 samples along each side
    uint32_t level_count;  // level 0 is the most detailed
    uint32_t root_count_x; // the number of nodes at the top level (level_count - 1), along x and y. The terrain is that many top
    uint32_t root_count_y; // level tiles
    uint32_t node_count;
    // The samples of level 0 that came from the source data: [min, max). The rest of the terrain is a smooth surface that fills
    // the space around them
    uint32_t data_min_x;
    uint32_t data_max_x;
    uint32_t data_min_y;
    uint32_t data_max_y;
    float sample_spacing; // metres between samples of level 0
    float height_offset;  // a sample's height is height_offset + its 16 bit value * height_scale
    float height_scale;
    float min_height;
    float max_height;
    uint32_t reserved;
};
static_assert(sizeof(TerrainHeader) == 64);

constexpr uint32_t TERRAIN_NODE_HAS_DATA = 1 << 0; // some of the node's samples came from the source data

// A node of the quadtree, as it is in the asset. A node's position isn't stored, as it is known by whoever got to the node from
// its parent (see TerrainNodeRef).
struct TerrainNode {
    float min_height; // of everything under the node, at every level of detail
    float max_height;
    // How far below its edges a tile's skirts hang. Neighbouring tiles that are drawn at different levels of detail don't
    // quite meet. A skirt is a strip of triangles that hangs down from a tile's edges, under the ground, to hide the gaps.
    float skirt_depth;
    uint32_t children[4]; // indices of the nodes, or TERRAIN_NO_NODE. In the order (-x, -y), (+x, -y), (-x, +y), (+x, +y)
    uint32_t flags;
};
static_assert(sizeof(TerrainNode) == 32);

// A node and where it is
struct TerrainNodeRef {
    uint32_t index{TERRAIN_NO_NODE};
    uint32_t level{};
    uint32_t x{}; // among the nodes of its level
    uint32_t y{};
};

// Decodes one sample of a tile
struct TerrainSample {
    uint32_t bits;

    uint32_t height() const { return bits & 0xffff; }
    glm::vec3 normal() const;
};

// A TERRAIN asset. The views point into the asset's data, which has to outlive the resource, as an asset in the content does.
struct ResourceTerrain {
    TerrainHeader header{};
    std::span<const TerrainNode> nodes{};
    std::span<const uint32_t> samples{}; // every node's tile, in the order of the nodes

    // Empty if the data isn't a TERRAIN asset of this version, or if the tree doesn't fit in it.
    static std::optional<ResourceTerrain> create(std::span<const uint8_t> asset_data);

    static std::optional<ResourceTerrain> create(const Content& content_manager, Name name);

    uint32_t getTopLevel() const { return header.level_count - 1; }
    uint32_t getTileSide() const { return header.tile_quads + 1; } // samples along a side
    size_t getTileSampleCount() const { return static_cast<size_t>(getTileSide()) * getTileSide(); }
    std::span<const uint32_t> getTile(uint32_t node) const { return samples.subspan(node * getTileSampleCount(), getTileSampleCount()); }
    float getCellSize(uint32_t level) const { return header.sample_spacing * static_cast<float>(1u << level); }
    float getNodeSize(uint32_t level) const { return getCellSize(level) * static_cast<float>(header.tile_quads); }
    float decodeHeight(uint32_t sample) const { return header.height_offset + static_cast<float>(sample & 0xffff) * header.height_scale; }

    // The size of the whole terrain, in metres
    glm::vec2 getSize() const;
    // The part of the terrain whose heights came from the source data, in metres
    glm::vec2 getDataMin() const;
    glm::vec2 getDataMax() const;

    TerrainNodeRef getRoot(uint32_t x, uint32_t y) const;
    TerrainNodeRef getChild(const TerrainNodeRef& node, uint32_t child) const; // index is TERRAIN_NO_NODE if there isn't one

    AABB getBounds() const;
    AABB getNodeBounds(const TerrainNodeRef& node) const;

    // The most detailed node at a point. Empty outside the terrain
    std::optional<TerrainNodeRef> findNode(float x, float y) const;

    // The height of the ground at a point, from the most detailed tile there. Empty outside the terrain
    std::optional<float> getHeight(float x, float y) const;

    // The nearest point where a ray hits the ground, from either side, within max_t (in units of the direction's length).
    // normal_out is the normal of the triangle that was hit, facing up.
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float max_t, float& t_out, glm::vec3& normal_out) const;

    // Appends the triangles of the ground (three corners each, counter-clockwise from above) that might touch a box: those of
    // the cells that the box covers, from the most detailed tile of each, that have a corner within the box's heights or that
    // span them.
    void getTriangles(const AABB& box, std::vector<glm::vec3>& corners_out) const;

private:
    // Appends the triangles of the cells [x0, x1) x [y0, y1) of a node's tile
    void appendCellTriangles(const TerrainNodeRef& node, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const AABB& box,
                             std::vector<glm::vec3>& corners_out) const;
    // Walks a ray through the cells [x0, x1) x [y0, y1) of a node's tile, nearest first, until it hits something.
    bool raycastCells(const TerrainNodeRef& node, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const glm::vec3& origin,
                      const glm::vec3& direction, float t_enter, float t_exit, float& t_out, glm::vec3& normal_out) const;
};

} // namespace gc
