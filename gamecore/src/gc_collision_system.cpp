#include "gamecore/gc_collision_system.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <span>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_collider_component.h"
#include "gamecore/gc_collision_bvh_component.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_resources.h"
#include "gamecore/gc_terrain.h"
#include "gamecore/gc_terrain_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"

#include "gc_collision_math.h"

namespace gc {

namespace {

// Trees of triangles are balanced, so their depth is about log2 of the number of triangles
constexpr size_t MESH_STACK_SIZE = 64;

} // namespace

CollisionSystem::CollisionSystem(World& world, ResourceManager& resource_manager) : System(world), m_resource_manager(resource_manager) {}

void CollisionSystem::onUpdate([[maybe_unused]] FrameState& frame_state)
{
    ZoneScoped;

    ++m_update_count;

    findBakedMeshes(); // before the colliders are looked at, so that the meshes of a prefab that was just loaded are found

    // Compare every collider with what was known about it, and only redo what changed
    m_world.forEach<TransformComponent, ColliderComponent>([&](Entity entity, const TransformComponent& t, const ColliderComponent& c) {
        const auto [it, is_new] = m_colliders.try_emplace(entity);
        Collider& collider = it->second;
        collider.last_seen = m_update_count;

        bool changed = is_new;
        if (c.m_mesh != collider.mesh) {
            releaseShape(collider.mesh);
            collider.mesh = c.m_mesh;
            collider.shape = acquireShape(c.m_mesh);
            changed = true;
        }
        const glm::mat4 world_matrix = t.getWorldMatrix();
        if (!changed && world_matrix == collider.world_matrix) {
            return;
        }

        collider.world_matrix = world_matrix;
        collider.inverse_matrix = glm::inverse(world_matrix);

        std::optional<AABB> box{};
        if (collider.shape && !collider.shape->bvh.nodes.empty()) {
            box = transformBox(collider.shape->bvh.nodes[0].box, world_matrix);
        }
        collider.tree_node = placeLeaf(collider.tree_node, box, entity, false);
    });

    // Forget the colliders of entities that were deleted, or that lost their ColliderComponent
    for (auto it = m_colliders.begin(); it != m_colliders.end();) {
        Collider& collider = it->second;
        if (collider.last_seen != m_update_count) {
            if (collider.tree_node != -1) {
                removeLeaf(collider.tree_node);
                m_free_tree_nodes.push_back(collider.tree_node);
            }
            releaseShape(collider.mesh);
            it = m_colliders.erase(it);
        }
        else {
            ++it;
        }
    }

    updateTerrains();
}

void CollisionSystem::updateTerrains()
{
    if (m_world.isComponentRegistered(TerrainComponent::NAME)) {
        m_world.forEach<TransformComponent, TerrainComponent>([&](Entity entity, const TransformComponent& t, const TerrainComponent& c) {
            if (!c.m_collision) {
                return; // (forgotten below)
            }
            const auto [it, is_new] = m_terrains.try_emplace(entity);
            TerrainCollider& terrain = it->second;
            terrain.last_seen = m_update_count;

            bool changed = is_new;
            if (c.m_terrain != terrain.terrain) {
                terrain.terrain = c.m_terrain;
                terrain.resource = c.m_terrain.empty() ? nullptr : m_resource_manager.get<ResourceTerrain>(c.m_terrain);
                if (!terrain.resource && !c.m_terrain.empty()) {
                    GC_ERROR("Could not load the terrain {} of a TerrainComponent", c.m_terrain);
                }
                changed = true;
            }
            const glm::mat4 world_matrix = t.getWorldMatrix();
            if (!changed && world_matrix == terrain.world_matrix) {
                return;
            }
            terrain.world_matrix = world_matrix;
            terrain.inverse_matrix = glm::inverse(world_matrix);
            std::optional<AABB> box{};
            if (terrain.resource) {
                box = transformBox(terrain.resource->getBounds(), world_matrix);
            }
            terrain.tree_node = placeLeaf(terrain.tree_node, box, entity, true);
        });
    }

    for (auto it = m_terrains.begin(); it != m_terrains.end();) {
        if (it->second.last_seen != m_update_count) {
            placeLeaf(it->second.tree_node, std::nullopt, it->first, true);
            it = m_terrains.erase(it);
        }
        else {
            ++it;
        }
    }
}

int32_t CollisionSystem::placeLeaf(int32_t tree_node, const std::optional<AABB>& box, Entity entity, bool is_terrain)
{
    // A leaf is moved by taking it out of the tree and putting it back in
    if (tree_node != -1) {
        removeLeaf(tree_node);
    }
    if (!box) {
        if (tree_node != -1) {
            m_free_tree_nodes.push_back(tree_node);
        }
        return -1;
    }
    if (tree_node == -1) {
        tree_node = allocateTreeNode();
    }
    TreeNode& leaf = m_tree[tree_node];
    leaf.box = *box;
    leaf.child1 = -1;
    leaf.child2 = -1;
    leaf.entity = entity;
    leaf.is_terrain = is_terrain;
    insertLeaf(tree_node);
    return tree_node;
}

RaycastHit CollisionSystem::raycast(const Ray& ray, float max_distance, Entity ignore) const
{
    ZoneScoped;

    RaycastHit result{};
    const float length = glm::length(ray.direction);
    if (m_tree_root == -1 || !(length > 0.0f)) {
        return result;
    }
    const glm::vec3 direction = ray.direction / length;
    const glm::vec3 inverse_direction = 1.0f / direction;

    float nearest = max_distance;
    glm::vec3 local_normal{};
    const glm::mat4* hit_inverse_matrix = nullptr; // of what was hit

    std::vector<int32_t> stack{};
    stack.reserve(64);
    stack.push_back(m_tree_root);
    while (!stack.empty()) {
        const TreeNode& node = m_tree[stack.back()];
        stack.pop_back();

        float entry{};
        if (!rayHitsBox(ray.origin, inverse_direction, node.box, nearest, entry)) {
            continue; // also skips boxes that are further away than what has been hit already
        }

        if (node.child1 != -1) {
            // look in the nearer child first, as a hit in it can rule out the other one
            float entry1{}, entry2{};
            const bool hit1 = rayHitsBox(ray.origin, inverse_direction, m_tree[node.child1].box, nearest, entry1);
            const bool hit2 = rayHitsBox(ray.origin, inverse_direction, m_tree[node.child2].box, nearest, entry2);
            if (hit1 && hit2) {
                stack.push_back((entry1 < entry2) ? node.child2 : node.child1);
                stack.push_back((entry1 < entry2) ? node.child1 : node.child2);
            }
            else if (hit1) {
                stack.push_back(node.child1);
            }
            else if (hit2) {
                stack.push_back(node.child2);
            }
            continue;
        }

        if (node.entity == ignore) {
            continue;
        }

        if (node.is_terrain) {
            // The terrain finds its own nearest hit, in its own space
            const TerrainCollider& terrain = m_terrains.at(node.entity);
            const glm::vec3 local_origin = glm::vec3(terrain.inverse_matrix * glm::vec4(ray.origin, 1.0f));
            const glm::vec3 local_direction = glm::mat3(terrain.inverse_matrix) * direction;
            float t{};
            glm::vec3 normal{};
            if (terrain.resource->raycast(local_origin, local_direction, nearest, t, normal) && t < nearest) {
                nearest = t;
                local_normal = normal;
                hit_inverse_matrix = &terrain.inverse_matrix;
                result.entity = node.entity;
            }
            continue;
        }

        // Narrow phase: the triangles of the collider's mesh. The ray is moved into the mesh's space, rather than the triangles
        // into the world's. Distances along the ray are the same in both, as the direction isn't normalised again.
        const Collider& collider = m_colliders.at(node.entity);
        const CollisionBvhView& shape = collider.shape->bvh;
        const glm::vec3 local_origin = glm::vec3(collider.inverse_matrix * glm::vec4(ray.origin, 1.0f));
        const glm::vec3 local_direction = glm::mat3(collider.inverse_matrix) * direction;
        const glm::vec3 local_inverse_direction = 1.0f / local_direction;

        std::array<uint32_t, MESH_STACK_SIZE> mesh_stack{};
        size_t mesh_stack_size = 0;
        mesh_stack[mesh_stack_size++] = 0;
        while (mesh_stack_size > 0) {
            const CollisionBvhNode& mesh_node = shape.nodes[mesh_stack[--mesh_stack_size]];
            float mesh_entry{};
            if (!rayHitsBox(local_origin, local_inverse_direction, mesh_node.box, nearest, mesh_entry)) {
                continue;
            }
            if (mesh_node.count == 0) {
                if (mesh_stack_size + 2 <= mesh_stack.size()) {
                    mesh_stack[mesh_stack_size++] = mesh_node.first;
                    mesh_stack[mesh_stack_size++] = mesh_node.first + 1;
                }
                continue;
            }
            for (uint32_t i = 0; i < mesh_node.count; ++i) {
                const uint16_t* const indices = &shape.triangles[static_cast<size_t>(mesh_node.first + i) * 3];
                const glm::vec3& a = shape.positions[indices[0]];
                const glm::vec3& b = shape.positions[indices[1]];
                const glm::vec3& c = shape.positions[indices[2]];
                float t{};
                if (rayHitsTriangle(local_origin, local_direction, a, b, c, t) && t < nearest) {
                    nearest = t;
                    local_normal = glm::cross(b - a, c - a);
                    hit_inverse_matrix = &collider.inverse_matrix;
                    result.entity = node.entity;
                }
            }
        }
    }

    if (hit_inverse_matrix) {
        result.hit = true;
        result.distance = nearest;
        result.position = ray.origin + direction * nearest;
        // normals are transformed by the inverse transpose
        result.normal = glm::normalize(glm::transpose(glm::mat3(*hit_inverse_matrix)) * local_normal);
        if (glm::dot(result.normal, direction) > 0.0f) {
            result.normal = -result.normal;
        }
    }
    return result;
}

void CollisionSystem::overlapSphere(const glm::vec3& centre, float radius, std::vector<SphereContact>& contacts_out, Entity ignore) const
{
    ZoneScoped;

    if (m_tree_root == -1 || !(radius > 0.0f)) {
        return;
    }
    const AABB sphere_box{centre - glm::vec3{radius}, centre + glm::vec3{radius}};

    std::vector<int32_t> stack{};
    stack.reserve(64);
    stack.push_back(m_tree_root);
    while (!stack.empty()) {
        const TreeNode& node = m_tree[stack.back()];
        stack.pop_back();
        if (!boxesOverlap(node.box, sphere_box)) {
            continue;
        }
        if (node.child1 != -1) {
            stack.push_back(node.child1);
            stack.push_back(node.child2);
            continue;
        }
        if (node.entity == ignore) {
            continue;
        }

        float nearest_squared = radius * radius;
        glm::vec3 nearest_point{};
        glm::vec3 nearest_triangle_normal{};
        bool found = false;
        const auto test_triangle = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            const glm::vec3 point = closestPointOnTriangle(centre, a, b, c);
            const glm::vec3 to_centre = centre - point;
            const float distance_squared = glm::dot(to_centre, to_centre);
            if (distance_squared < nearest_squared) {
                nearest_squared = distance_squared;
                nearest_point = point;
                nearest_triangle_normal = glm::cross(b - a, c - a);
                found = true;
            }
        };

        if (node.is_terrain) {
            const TerrainCollider& terrain = m_terrains.at(node.entity);
            const glm::vec3 local_centre = glm::vec3(terrain.inverse_matrix * glm::vec4(centre, 1.0f));
            const std::optional<float> ground = terrain.resource->getHeight(local_centre.x, local_centre.y);
            if (ground && local_centre.z < *ground) {
                // Under the ground: a terrain is solid all the way down, so the sphere is pushed straight up out of it (its
                // nearest triangle could be anywhere, or out of reach)
                SphereContact contact{};
                contact.entity = node.entity;
                contact.position = glm::vec3(terrain.world_matrix * glm::vec4(local_centre.x, local_centre.y, *ground, 1.0f));
                contact.normal = glm::normalize(glm::vec3(terrain.world_matrix[2]));
                contact.depth = radius + (*ground - local_centre.z);
                contacts_out.push_back(contact);
                continue;
            }
            // (like a mesh's, the triangles are tested in world space)
            std::vector<glm::vec3> corners{};
            terrain.resource->getTriangles(transformBox(sphere_box, terrain.inverse_matrix), corners);
            for (size_t i = 0; i + 2 < corners.size(); i += 3) {
                test_triangle(glm::vec3(terrain.world_matrix * glm::vec4(corners[i], 1.0f)), glm::vec3(terrain.world_matrix * glm::vec4(corners[i + 1], 1.0f)),
                              glm::vec3(terrain.world_matrix * glm::vec4(corners[i + 2], 1.0f)));
            }
        }
        else {
            // Narrow phase. The triangles near the sphere are found in the mesh's space, then tested in world space: the sphere
            // wouldn't be a sphere in the mesh's space if the entity is scaled.
            const Collider& collider = m_colliders.at(node.entity);
            const CollisionBvhView& shape = collider.shape->bvh;
            const AABB local_box = transformBox(sphere_box, collider.inverse_matrix);

            std::array<uint32_t, MESH_STACK_SIZE> mesh_stack{};
            size_t mesh_stack_size = 0;
            mesh_stack[mesh_stack_size++] = 0;
            while (mesh_stack_size > 0) {
                const CollisionBvhNode& mesh_node = shape.nodes[mesh_stack[--mesh_stack_size]];
                if (!boxesOverlap(mesh_node.box, local_box)) {
                    continue;
                }
                if (mesh_node.count == 0) {
                    if (mesh_stack_size + 2 <= mesh_stack.size()) {
                        mesh_stack[mesh_stack_size++] = mesh_node.first;
                        mesh_stack[mesh_stack_size++] = mesh_node.first + 1;
                    }
                    continue;
                }
                for (uint32_t i = 0; i < mesh_node.count; ++i) {
                    const uint16_t* const indices = &shape.triangles[static_cast<size_t>(mesh_node.first + i) * 3];
                    test_triangle(glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[0]], 1.0f)),
                                  glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[1]], 1.0f)),
                                  glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[2]], 1.0f)));
                }
            }
        }

        if (found) {
            const float distance = std::sqrt(nearest_squared);
            SphereContact contact{};
            contact.entity = node.entity;
            contact.position = nearest_point;
            contact.depth = radius - distance;
            if (distance > 1.0e-6f) {
                contact.normal = (centre - nearest_point) / distance;
            }
            else if (glm::dot(nearest_triangle_normal, nearest_triangle_normal) > 0.0f) {
                // the centre is on the surface, so there is no direction to it. Use the way the triangle faces
                contact.normal = glm::normalize(nearest_triangle_normal);
            }
            else {
                contact.normal = glm::vec3{0.0f, 0.0f, 1.0f};
            }
            contacts_out.push_back(contact);
        }
    }
}

void CollisionSystem::queryAABB(const AABB& box, std::vector<Entity>& entities_out) const
{
    if (m_tree_root == -1) {
        return;
    }
    std::vector<int32_t> stack{};
    stack.reserve(64);
    stack.push_back(m_tree_root);
    while (!stack.empty()) {
        const TreeNode& node = m_tree[stack.back()];
        stack.pop_back();
        if (!boxesOverlap(node.box, box)) {
            continue;
        }
        if (node.child1 != -1) {
            stack.push_back(node.child1);
            stack.push_back(node.child2);
        }
        else {
            entities_out.push_back(node.entity);
        }
    }
}

std::optional<AABB> CollisionSystem::getColliderAABB(Entity entity) const
{
    if (const auto it = m_colliders.find(entity); it != m_colliders.end() && it->second.tree_node != -1) {
        return m_tree[it->second.tree_node].box;
    }
    if (const auto it = m_terrains.find(entity); it != m_terrains.end() && it->second.tree_node != -1) {
        return m_tree[it->second.tree_node].box;
    }
    return {};
}

int CollisionSystem::getTreeHeight() const
{
    if (m_tree_root == -1) {
        return 0;
    }
    int height = 0;
    std::vector<std::pair<int32_t, int>> stack{{m_tree_root, 1}};
    while (!stack.empty()) {
        const auto [index, depth] = stack.back();
        stack.pop_back();
        height = std::max(height, depth);
        if (m_tree[index].child1 != -1) {
            stack.emplace_back(m_tree[index].child1, depth + 1);
            stack.emplace_back(m_tree[index].child2, depth + 1);
        }
    }
    return height;
}

//
// Meshes
//

size_t CollisionSystem::getBakedMeshCount() const
{
    return static_cast<size_t>(std::count_if(m_shapes.begin(), m_shapes.end(), [](const auto& shape) { return shape.second.is_baked; }));
}

// Keeps m_baked_meshes up to date with the CollisionBvhComponents in the world. There are only ever a few of them, and they only
// change when a prefab is loaded or deleted.
void CollisionSystem::findBakedMeshes()
{
    if (!m_world.isComponentRegistered(CollisionBvhComponent::NAME)) {
        return; // the game doesn't use baked trees
    }

    std::vector<Name> assets{};
    m_world.forEach<CollisionBvhComponent>([&](Entity, const CollisionBvhComponent& c) {
        if (!c.m_bvh.empty() && std::find(assets.begin(), assets.end(), c.m_bvh) == assets.end()) {
            assets.push_back(c.m_bvh);
        }
    });
    if (assets == m_baked_assets) {
        return;
    }

    // Shapes that are already in use keep the trees they have. An asset's data stays where it is after its component has gone.
    m_baked_meshes.clear();
    for (const Name asset : assets) {
        const ResourceCollisionBvh* const resource = m_resource_manager.get<ResourceCollisionBvh>(asset);
        if (!resource) {
            if (std::find(m_baked_assets.begin(), m_baked_assets.end(), asset) == m_baked_assets.end()) {
                GC_ERROR("Could not load the baked collision trees {}. The trees of its meshes will be built instead", asset);
            }
            continue;
        }
        for (const auto& [mesh, bvh] : resource->meshes) {
            m_baked_meshes.try_emplace(mesh, bvh);
        }
    }
    m_baked_assets = std::move(assets);
    GC_DEBUG("Using the baked collision trees of {} meshes, from {} assets", m_baked_meshes.size(), m_baked_assets.size());
}

// Returns null if the name is empty. Call releaseShape() with the same name either way.
const CollisionSystem::MeshShape* CollisionSystem::acquireShape(Name mesh)
{
    if (mesh.empty()) {
        return nullptr;
    }
    if (const auto it = m_shapes.find(mesh); it != m_shapes.end()) {
        it->second.ref_count += 1;
        return &it->second;
    }

    // (shapes stay where they are in the map, so a shape's view of the tree that it built itself stays valid)
    MeshShape& shape = m_shapes[mesh];
    shape.ref_count = 1;

    if (const auto it = m_baked_meshes.find(mesh); it != m_baked_meshes.end()) {
        shape.bvh = it->second; // the mesh itself isn't needed
        shape.is_baked = true;
        return &shape;
    }

    // A missing mesh gets an (empty) shape too, so that it is only looked for once, however many colliders want it
    const ResourceMesh* const resource = m_resource_manager.get<ResourceMesh>(mesh);
    if (!resource) {
        GC_ERROR("Could not find the mesh of a collider: {}", mesh);
        return &shape;
    }
    shape.built = buildCollisionBvh(resource->vertices.get(), resource->indices.get());
    shape.bvh = shape.built;
    return &shape;
}

void CollisionSystem::releaseShape(Name mesh)
{
    if (const auto it = m_shapes.find(mesh); it != m_shapes.end()) {
        it->second.ref_count -= 1;
        if (it->second.ref_count <= 0) {
            GC_ASSERT(it->second.ref_count == 0);
            m_shapes.erase(it);
        }
    }
}

//
// The broad phase tree. Leaves are added and removed one at a time, as Box2D's b2DynamicTree does.
//

int32_t CollisionSystem::allocateTreeNode()
{
    if (!m_free_tree_nodes.empty()) {
        const int32_t node = m_free_tree_nodes.back();
        m_free_tree_nodes.pop_back();
        return node;
    }
    m_tree.emplace_back();
    return static_cast<int32_t>(m_tree.size()) - 1;
}

// The leaf's box, entity and (absent) children must be set already
void CollisionSystem::insertLeaf(int32_t leaf)
{
    if (m_tree_root == -1) {
        m_tree_root = leaf;
        m_tree[leaf].parent = -1;
        return;
    }

    // Find the node to put the leaf next to: go down the tree, each time into the child whose box would have to grow the least
    // (in surface area) to hold the leaf, until making the leaf a sibling of the current node is cheaper than going further.
    const AABB leaf_box = m_tree[leaf].box;
    int32_t sibling = m_tree_root;
    while (m_tree[sibling].child1 != -1) {
        const TreeNode& node = m_tree[sibling];
        const float area = boxArea(node.box);
        const float combined_area = boxArea(boxUnion(node.box, leaf_box));

        // the cost of making a new parent for this node and the leaf
        const float cost = 2.0f * combined_area;
        // what every node below this one costs more if this node's box has to grow
        const float inheritance_cost = 2.0f * (combined_area - area);

        const auto child_cost = [&](int32_t child) {
            const TreeNode& child_node = m_tree[child];
            const float new_area = boxArea(boxUnion(child_node.box, leaf_box));
            if (child_node.child1 == -1) {
                return new_area + inheritance_cost;
            }
            return (new_area - boxArea(child_node.box)) + inheritance_cost;
        };
        const float cost1 = child_cost(node.child1);
        const float cost2 = child_cost(node.child2);
        if (cost < cost1 && cost < cost2) {
            break;
        }
        sibling = (cost1 < cost2) ? node.child1 : node.child2;
    }

    // put a new parent where the sibling was, with the sibling and the leaf as its children
    const int32_t old_parent = m_tree[sibling].parent;
    const int32_t new_parent = allocateTreeNode(); // (may move the nodes)
    m_tree[new_parent].parent = old_parent;
    m_tree[new_parent].child1 = sibling;
    m_tree[new_parent].child2 = leaf;
    m_tree[new_parent].entity = ENTITY_NONE;
    m_tree[new_parent].is_terrain = false;
    m_tree[sibling].parent = new_parent;
    m_tree[leaf].parent = new_parent;
    if (old_parent == -1) {
        m_tree_root = new_parent;
    }
    else if (m_tree[old_parent].child1 == sibling) {
        m_tree[old_parent].child1 = new_parent;
    }
    else {
        m_tree[old_parent].child2 = new_parent;
    }

    refitAncestors(new_parent);
}

// Takes the leaf out of the tree. Its node isn't freed, so that it can be put back in with a different box.
void CollisionSystem::removeLeaf(int32_t leaf)
{
    if (leaf == m_tree_root) {
        m_tree_root = -1;
        return;
    }

    // the leaf's sibling takes the place of their parent
    const int32_t parent = m_tree[leaf].parent;
    const int32_t grandparent = m_tree[parent].parent;
    const int32_t sibling = (m_tree[parent].child1 == leaf) ? m_tree[parent].child2 : m_tree[parent].child1;
    m_tree[sibling].parent = grandparent;
    if (grandparent == -1) {
        m_tree_root = sibling;
    }
    else {
        if (m_tree[grandparent].child1 == parent) {
            m_tree[grandparent].child1 = sibling;
        }
        else {
            m_tree[grandparent].child2 = sibling;
        }
        refitAncestors(grandparent);
    }
    m_free_tree_nodes.push_back(parent);
    m_tree[leaf].parent = -1;
}

// Makes the boxes of a node and everything above it fit their children again
void CollisionSystem::refitAncestors(int32_t node)
{
    while (node != -1) {
        m_tree[node].box = boxUnion(m_tree[m_tree[node].child1].box, m_tree[m_tree[node].child2].box);
        node = m_tree[node].parent;
    }
}

} // namespace gc
