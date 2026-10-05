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
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_resources.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"

namespace gc {

namespace {

constexpr uint32_t MAX_TRIANGLES_PER_LEAF = 4;

AABB boxUnion(const AABB& a, const AABB& b) { return AABB{glm::min(a.min, b.min), glm::max(a.max, b.max)}; }

void boxExpand(AABB& box, const glm::vec3& point)
{
    box.min = glm::min(box.min, point);
    box.max = glm::max(box.max, point);
}

// the surface area: boxes with less of it are hit by fewer rays, which is what makes a good tree
float boxArea(const AABB& box)
{
    const glm::vec3 size = box.max - box.min;
    return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
}

bool boxesOverlap(const AABB& a, const AABB& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// the box around a box that has been transformed
AABB transformBox(const AABB& box, const glm::mat4& matrix)
{
    AABB result{};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z};
        boxExpand(result, glm::vec3(matrix * glm::vec4(point, 1.0f)));
    }
    return result;
}

// Whether the ray gets into the box before it has gone max_t. entry_t is how far along the ray it does (zero if it starts inside).
// inverse_direction is 1 / direction, which is infinite for the axes the ray doesn't move along.
bool rayHitsBox(const glm::vec3& origin, const glm::vec3& inverse_direction, const AABB& box, float max_t, float& entry_t)
{
    // Thank you https://tavianator.com/cgit/dimension.git/tree/libdimension/bvh/bvh.c
    float t_min = 0.0f;
    float t_max = max_t;
    for (int axis = 0; axis < 3; ++axis) {
        const float t1 = (box.min[axis] - origin[axis]) * inverse_direction[axis];
        const float t2 = (box.max[axis] - origin[axis]) * inverse_direction[axis];
        // written so that a NaN (zero times infinity, when the ray lies in one of the box's faces) leaves the range alone
        t_min = std::max(t_min, std::min(t1, t2));
        t_max = std::min(t_max, std::max(t1, t2));
    }
    entry_t = t_min;
    return t_max >= t_min;
}

// Moller-Trumbore. Hits either side of the triangle. t is in units of the direction's length.
bool rayHitsTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t)
{
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 p = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (determinant == 0.0f) {
        return false; // parallel to the triangle, or the triangle has no area
    }
    const float inverse_determinant = 1.0f / determinant;
    const glm::vec3 to_origin = origin - a;
    const float u = glm::dot(to_origin, p) * inverse_determinant;
    if (!(u >= 0.0f && u <= 1.0f)) {
        return false;
    }
    const glm::vec3 q = glm::cross(to_origin, edge1);
    const float v = glm::dot(direction, q) * inverse_determinant;
    if (!(v >= 0.0f && u + v <= 1.0f)) {
        return false;
    }
    t = glm::dot(edge2, q) * inverse_determinant;
    return t >= 0.0f;
}

// From "Real-Time Collision Detection" by Christer Ericson
glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denominator = va + vb + vc;
    if (denominator == 0.0f) {
        return a; // the triangle has no area
    }
    return a + ab * (vb / denominator) + ac * (vc / denominator);
}

// Trees of triangles are balanced, so their depth is about log2 of the number of triangles
constexpr size_t MESH_STACK_SIZE = 64;

} // namespace

CollisionSystem::CollisionSystem(World& world, ResourceManager& resource_manager) : System(world), m_resource_manager(resource_manager) {}

void CollisionSystem::onUpdate([[maybe_unused]] FrameState& frame_state)
{
    ZoneScoped;

    ++m_update_count;

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

        // Move the collider's box in the broad phase tree by taking it out and putting it back in
        if (collider.tree_node != -1) {
            removeLeaf(collider.tree_node);
        }
        if (collider.shape && !collider.shape->nodes.empty()) {
            if (collider.tree_node == -1) {
                collider.tree_node = allocateTreeNode();
            }
            TreeNode& leaf = m_tree[collider.tree_node];
            leaf.box = transformBox(collider.shape->nodes[0].box, world_matrix);
            leaf.child1 = -1;
            leaf.child2 = -1;
            leaf.entity = entity;
            insertLeaf(collider.tree_node);
        }
        else if (collider.tree_node != -1) {
            m_free_tree_nodes.push_back(collider.tree_node);
            collider.tree_node = -1;
        }
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
    const Collider* hit_collider = nullptr;

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

        // Narrow phase: the triangles of the collider's mesh. The ray is moved into the mesh's space, rather than the triangles
        // into the world's. Distances along the ray are the same in both, as the direction isn't normalised again.
        const Collider& collider = m_colliders.at(node.entity);
        const MeshShape& shape = *collider.shape;
        const glm::vec3 local_origin = glm::vec3(collider.inverse_matrix * glm::vec4(ray.origin, 1.0f));
        const glm::vec3 local_direction = glm::mat3(collider.inverse_matrix) * direction;
        const glm::vec3 local_inverse_direction = 1.0f / local_direction;

        std::array<uint32_t, MESH_STACK_SIZE> mesh_stack{};
        size_t mesh_stack_size = 0;
        mesh_stack[mesh_stack_size++] = 0;
        while (mesh_stack_size > 0) {
            const MeshNode& mesh_node = shape.nodes[mesh_stack[--mesh_stack_size]];
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
                const uint32_t* const indices = &shape.triangles[static_cast<size_t>(mesh_node.first + i) * 3];
                const glm::vec3& a = shape.positions[indices[0]];
                const glm::vec3& b = shape.positions[indices[1]];
                const glm::vec3& c = shape.positions[indices[2]];
                float t{};
                if (rayHitsTriangle(local_origin, local_direction, a, b, c, t) && t < nearest) {
                    nearest = t;
                    local_normal = glm::cross(b - a, c - a);
                    hit_collider = &collider;
                    result.entity = node.entity;
                }
            }
        }
    }

    if (hit_collider) {
        result.hit = true;
        result.distance = nearest;
        result.position = ray.origin + direction * nearest;
        // normals are transformed by the inverse transpose
        result.normal = glm::normalize(glm::transpose(glm::mat3(hit_collider->inverse_matrix)) * local_normal);
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

        // Narrow phase. The triangles near the sphere are found in the mesh's space, then tested in world space: the sphere
        // wouldn't be a sphere in the mesh's space if the entity is scaled.
        const Collider& collider = m_colliders.at(node.entity);
        const MeshShape& shape = *collider.shape;
        const AABB local_box = transformBox(sphere_box, collider.inverse_matrix);

        float nearest_squared = radius * radius;
        glm::vec3 nearest_point{};
        glm::vec3 nearest_triangle_normal{};
        bool found = false;

        std::array<uint32_t, MESH_STACK_SIZE> mesh_stack{};
        size_t mesh_stack_size = 0;
        mesh_stack[mesh_stack_size++] = 0;
        while (mesh_stack_size > 0) {
            const MeshNode& mesh_node = shape.nodes[mesh_stack[--mesh_stack_size]];
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
                const uint32_t* const indices = &shape.triangles[static_cast<size_t>(mesh_node.first + i) * 3];
                const glm::vec3 a = glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[0]], 1.0f));
                const glm::vec3 b = glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[1]], 1.0f));
                const glm::vec3 c = glm::vec3(collider.world_matrix * glm::vec4(shape.positions[indices[2]], 1.0f));
                const glm::vec3 point = closestPointOnTriangle(centre, a, b, c);
                const glm::vec3 to_centre = centre - point;
                const float distance_squared = glm::dot(to_centre, to_centre);
                if (distance_squared < nearest_squared) {
                    nearest_squared = distance_squared;
                    nearest_point = point;
                    nearest_triangle_normal = glm::cross(b - a, c - a);
                    found = true;
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

// Returns null if there is no such mesh, or it has no triangles. Call releaseShape() with the same name either way.
const CollisionSystem::MeshShape* CollisionSystem::acquireShape(Name mesh)
{
    if (mesh.empty()) {
        return nullptr;
    }
    if (const auto it = m_shapes.find(mesh); it != m_shapes.end()) {
        it->second.ref_count += 1;
        return &it->second;
    }

    const ResourceMesh* const resource = m_resource_manager.get<ResourceMesh>(mesh);
    if (!resource) {
        GC_ERROR("Could not find the mesh of a collider: {}", mesh);
    }

    // A missing mesh gets an (empty) shape too, so that it is only looked for once, however many colliders want it
    MeshShape& shape = m_shapes[mesh];
    shape.ref_count = 1;
    if (resource) {
        const std::span<const MeshVertex> vertices = resource->vertices.get();
        const std::span<const uint16_t> indices = resource->indices.get();
        shape.positions.reserve(vertices.size());
        for (const MeshVertex& vertex : vertices) {
            shape.positions.push_back(vertex.position);
        }
        shape.triangles.reserve(indices.size());
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            if (indices[i] < vertices.size() && indices[i + 1] < vertices.size() && indices[i + 2] < vertices.size()) {
                shape.triangles.push_back(indices[i]);
                shape.triangles.push_back(indices[i + 1]);
                shape.triangles.push_back(indices[i + 2]);
            }
        }
        buildMeshShape(shape);
    }
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

// Sorts the shape's triangles into a tree. A mesh never changes, so the tree is built once, from the top down: the triangles are
// split in half along the axis they are most spread out on, and each half is split again until only a few are left.
void CollisionSystem::buildMeshShape(MeshShape& shape)
{
    ZoneScoped;

    const uint32_t triangle_count = static_cast<uint32_t>(shape.triangles.size() / 3);
    if (triangle_count == 0) {
        return;
    }

    struct Triangle {
        AABB box;
        glm::vec3 centroid;
        uint32_t index;
    };
    std::vector<Triangle> triangles(triangle_count);
    for (uint32_t i = 0; i < triangle_count; ++i) {
        Triangle& triangle = triangles[i];
        for (int corner = 0; corner < 3; ++corner) {
            boxExpand(triangle.box, shape.positions[shape.triangles[static_cast<size_t>(i) * 3 + corner]]);
        }
        triangle.centroid = 0.5f * (triangle.box.min + triangle.box.max);
        triangle.index = i;
    }

    shape.nodes.reserve(static_cast<size_t>(triangle_count) * 2 / MAX_TRIANGLES_PER_LEAF + 1);
    shape.nodes.emplace_back();

    // ranges of 'triangles' that still have to be made into nodes
    struct Work {
        uint32_t node;
        uint32_t first;
        uint32_t count;
    };
    std::vector<Work> work{{0, 0, triangle_count}};
    while (!work.empty()) {
        const Work range = work.back();
        work.pop_back();

        AABB box{};
        AABB centroid_box{};
        for (uint32_t i = range.first; i < range.first + range.count; ++i) {
            box = boxUnion(box, triangles[i].box);
            boxExpand(centroid_box, triangles[i].centroid);
        }
        shape.nodes[range.node].box = box;

        if (range.count <= MAX_TRIANGLES_PER_LEAF) {
            shape.nodes[range.node].first = range.first;
            shape.nodes[range.node].count = range.count;
            continue;
        }

        const glm::vec3 spread = centroid_box.max - centroid_box.min;
        const int axis = (spread.x >= spread.y && spread.x >= spread.z) ? 0 : ((spread.y >= spread.z) ? 1 : 2);
        const uint32_t half = range.count / 2;
        std::nth_element(triangles.begin() + range.first, triangles.begin() + range.first + half, triangles.begin() + range.first + range.count,
                         [axis](const Triangle& a, const Triangle& b) { return a.centroid[axis] < b.centroid[axis]; });

        const uint32_t children = static_cast<uint32_t>(shape.nodes.size());
        shape.nodes.emplace_back();
        shape.nodes.emplace_back();
        shape.nodes[range.node].first = children;
        shape.nodes[range.node].count = 0;
        work.push_back(Work{children, range.first, half});
        work.push_back(Work{children + 1, range.first + half, range.count - half});
    }

    // put the triangles in the order of the leaves
    std::vector<uint32_t> sorted(shape.triangles.size());
    for (uint32_t i = 0; i < triangle_count; ++i) {
        for (int corner = 0; corner < 3; ++corner) {
            sorted[static_cast<size_t>(i) * 3 + corner] = shape.triangles[static_cast<size_t>(triangles[i].index) * 3 + corner];
        }
    }
    shape.triangles = std::move(sorted);
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
