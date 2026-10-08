#pragma once

// The triangles of a mesh, sorted into a tree of bounding boxes (a BVH), which is what the CollisionSystem's narrow phase tests.
//
// A tree can be built when it is needed (buildCollisionBvh()), but that takes time when a world is loaded, so the trees of a
// prefab's meshes are usually baked beforehand: gcpak_editor builds the tree of every mesh that the prefab's colliders use and
// puts them all in one COLLISION_BVH asset, which the prefab names with a CollisionBvhComponent. Nothing is worked out when that
// asset is loaded: the CollisionSystem reads the trees where they are in the mapped .gcpak file.
//
// A tree has its own copy of the mesh's positions, so the mesh itself isn't needed (or even read) to collide with it.

#include <cstdint>

#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "gamecore/gc_content.h"
#include "gamecore/gc_mesh_vertex.h"
#include "gamecore/gc_name.h"

namespace gc {

struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
};

// One node of a mesh's tree. This is also how nodes are stored in a COLLISION_BVH asset.
struct CollisionBvhNode {
    AABB box;
    uint32_t first; // if count is zero, the index of the first of the node's two children (the other one follows it).
                    // Otherwise the node is a leaf, and this is its first triangle
    uint32_t count;
};
static_assert(sizeof(glm::vec3) == 3 * sizeof(float));
static_assert(sizeof(CollisionBvhNode) == 8 * sizeof(uint32_t));

// A tree that was built here, rather than read from an asset
struct CollisionBvh {
    std::vector<glm::vec3> positions{};
    std::vector<uint16_t> triangles{};     // three indices into positions for each triangle, in the order of the tree's leaves
    std::vector<CollisionBvhNode> nodes{}; // nodes[0] is the root. Empty if the mesh has no triangles
};

// A tree that is somewhere else: in a CollisionBvh, or in the data of an asset
struct CollisionBvhView {
    std::span<const glm::vec3> positions{};
    std::span<const uint16_t> triangles{};
    std::span<const CollisionBvhNode> nodes{};

    CollisionBvhView() = default;
    CollisionBvhView(const CollisionBvh& bvh) : positions(bvh.positions), triangles(bvh.triangles), nodes(bvh.nodes) {}
};

// Sorts a mesh's triangles into a tree. Triangles that refer to vertices that don't exist are left out.
CollisionBvh buildCollisionBvh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices);

// The data of a COLLISION_BVH asset (see gcpak.h): the trees of several meshes, each with the ID of its mesh asset.
std::vector<uint8_t> makeCollisionBvhData(std::span<const std::pair<Name, CollisionBvh>> meshes);

// The baked trees of a prefab's meshes. See CollisionBvhComponent
struct ResourceCollisionBvh {
    std::unordered_map<Name, CollisionBvhView> meshes{}; // by the ID of the mesh asset. The views point into the asset's data

    // asset_data has to outlive the resource, as the data of an asset in the content does.
    // Empty if the data isn't a COLLISION_BVH asset of this version, or if its trees don't fit inside it. What is in the trees
    // (which triangles a node has, which positions a triangle uses) is trusted, as the vertices and indices of a mesh are.
    static std::optional<ResourceCollisionBvh> create(std::span<const uint8_t> asset_data);

    static std::optional<ResourceCollisionBvh> create(const Content& content_manager, Name name);
};

} // namespace gc
