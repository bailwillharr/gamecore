#pragma once

// Collision detection.
//
// Every entity with a ColliderComponent is a collider: the triangles of its mesh, wherever the entity's transform puts them.
// The system answers questions about them (what does this ray hit, what is this sphere touching). It doesn't move anything:
// what to do about a collision is up to the game.
//
// Queries have two phases:
//  - Broad phase. The world space bounding box of every collider is kept in a tree of bounding boxes (a BVH), so that a query
//    only looks at the colliders near it. Colliders are added to, moved in and removed from the tree as the world changes,
//    without rebuilding it.
//  - Narrow phase. The triangles of the colliders that the broad phase found are tested. Each mesh has its own tree of triangles,
//    built once and shared by every collider that uses the mesh.
//
// onUpdate() brings the system up to date with the world: new and deleted entities, changed meshes, moved transforms. Queries see
// the world as it was then, so register the system before the systems that use it (and after nothing that moves colliders, if
// that matters: otherwise queries are a frame behind for the things that moved).

#include <cstdint>

#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

namespace gc {

class World;           // forward-dec
class ResourceManager; // forward-dec
struct FrameState;     // forward-dec

struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
};

struct Ray {
    glm::vec3 origin{};
    glm::vec3 direction{}; // doesn't have to be normalised
};

struct RaycastHit {
    bool hit{false}; // if false, the rest is meaningless
    Entity entity{ENTITY_NONE};
    float distance{};     // from the ray's origin
    glm::vec3 position{}; // in world space
    glm::vec3 normal{};   // of the triangle that was hit, facing the ray's origin
};

// A collider that a sphere is touching
struct SphereContact {
    Entity entity{ENTITY_NONE};
    glm::vec3 position{}; // the point on the collider that is closest to the sphere's centre
    glm::vec3 normal{};   // from that point towards the sphere's centre. Moving the sphere by normal * depth separates them
    float depth{};        // how far the sphere is inside the collider's surface
};

class CollisionSystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("CollisionSystem");

private:
    // The triangles of a mesh, in a tree. Shared by the colliders that use the mesh
    struct MeshNode {
        AABB box;
        uint32_t first; // if count is zero, the index of the first of the node's two children (the other one follows it).
                        // Otherwise the node is a leaf, and this is its first triangle in MeshShape::triangles
        uint32_t count;
    };
    struct MeshShape {
        std::vector<glm::vec3> positions{};
        std::vector<uint32_t> triangles{}; // three indices into positions for each triangle, in the order of the tree's leaves
        std::vector<MeshNode> nodes{};     // nodes[0] is the root. Empty if the mesh has no triangles
        int ref_count{};
    };

    // What the system knows about one entity's collider. The ColliderComponent only names the mesh.
    struct Collider {
        Name mesh{};                // what the component had when it was last looked at
        const MeshShape* shape{};   // null if the mesh is missing or empty: nothing hits the collider then
        glm::mat4 world_matrix{};   // the transform that the rest was worked out from
        glm::mat4 inverse_matrix{}; // world space to the mesh's space
        int32_t tree_node{-1};      // the collider's leaf in the broad phase tree, or -1 if it isn't in it
        uint64_t last_seen{};       // the update in which the entity last had a ColliderComponent
    };

    // One node of the broad phase tree. A leaf is a collider. Any other node has two children and bounds both of them.
    struct TreeNode {
        AABB box;
        int32_t parent;
        int32_t child1; // -1 if the node is a leaf
        int32_t child2;
        Entity entity; // leaves only
    };

    ResourceManager& m_resource_manager;

    std::unordered_map<Name, MeshShape> m_shapes{};
    std::unordered_map<Entity, Collider> m_colliders{};

    std::vector<TreeNode> m_tree{};
    std::vector<int32_t> m_free_tree_nodes{};
    int32_t m_tree_root{-1};

    uint64_t m_update_count{};

public:
    CollisionSystem(World& world, ResourceManager& resource_manager);

    void onUpdate(FrameState& frame_state) override;

    // Finds the nearest thing that the ray hits within max_distance. Triangles are hit from both sides.
    // 'ignore' is an entity that the ray passes through, such as whoever is firing it.
    RaycastHit raycast(const Ray& ray, float max_distance = std::numeric_limits<float>::max(), Entity ignore = ENTITY_NONE) const;

    // Finds the colliders that a sphere is touching. One contact is appended for each collider: its deepest.
    void overlapSphere(const glm::vec3& centre, float radius, std::vector<SphereContact>& contacts_out, Entity ignore = ENTITY_NONE) const;

    // Broad phase only: appends the colliders whose bounding boxes touch the box.
    void queryAABB(const AABB& box, std::vector<Entity>& entities_out) const;

    // The world space bounding box of an entity's collider. Empty if it doesn't have one, or its mesh is missing.
    std::optional<AABB> getColliderAABB(Entity entity) const;

    // For debugging
    size_t getColliderCount() const { return m_colliders.size(); }
    size_t getMeshCount() const { return m_shapes.size(); }
    int getTreeHeight() const;

private:
    const MeshShape* acquireShape(Name mesh);
    void releaseShape(Name mesh);
    static void buildMeshShape(MeshShape& shape);

    int32_t allocateTreeNode();
    void insertLeaf(int32_t leaf);
    void removeLeaf(int32_t leaf);
    void refitAncestors(int32_t node);
};

} // namespace gc
