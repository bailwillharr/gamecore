#include "gamecore/gc_collision_bvh.h"

#include <cstring>

#include <algorithm>
#include <array>

#include <glm/common.hpp>

#include <tracy/Tracy.hpp>

#include <gcpak/gcpak.h>

namespace gc {

namespace {

constexpr uint32_t MAX_TRIANGLES_PER_LEAF = 4;

// The layout of a COLLISION_BVH asset. See gcpak.h
constexpr uint32_t ASSET_VERSION = 1;
constexpr size_t ASSET_HEADER_SIZE = 2 * sizeof(uint32_t); // version, number of meshes
struct AssetMeshEntry {
    uint32_t mesh_id;
    uint32_t position_count;
    uint32_t triangle_count;
    uint32_t node_count;
};
constexpr size_t TRIANGLE_SIZE = 3 * sizeof(uint16_t);

// the trees are read where they are in the asset, so everything in it is kept aligned to 4 bytes (from the start of the asset)
constexpr size_t alignUp(size_t size) { return (size + 3) & ~size_t{3}; }

} // namespace

// A mesh never changes, so the tree is built once, from the top down: the triangles are split in half along the axis they are
// most spread out on, and each half is split again until only a few are left.
CollisionBvh buildCollisionBvh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices)
{
    ZoneScoped;

    CollisionBvh bvh{};

    struct Triangle {
        AABB box;
        glm::vec3 centroid;
        std::array<uint16_t, 3> indices;
    };
    std::vector<Triangle> triangles{};
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() || indices[i + 2] >= vertices.size()) {
            continue;
        }
        Triangle triangle{};
        triangle.indices = {indices[i], indices[i + 1], indices[i + 2]};
        for (const uint16_t index : triangle.indices) {
            triangle.box.min = glm::min(triangle.box.min, vertices[index].position);
            triangle.box.max = glm::max(triangle.box.max, vertices[index].position);
        }
        triangle.centroid = 0.5f * (triangle.box.min + triangle.box.max);
        triangles.push_back(triangle);
    }
    const uint32_t triangle_count = static_cast<uint32_t>(triangles.size());
    if (triangle_count == 0) {
        return bvh;
    }

    bvh.positions.reserve(vertices.size());
    for (const MeshVertex& vertex : vertices) {
        bvh.positions.push_back(vertex.position);
    }

    bvh.nodes.reserve(static_cast<size_t>(triangle_count) * 2 / MAX_TRIANGLES_PER_LEAF + 1);
    bvh.nodes.emplace_back();

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
            box.min = glm::min(box.min, triangles[i].box.min);
            box.max = glm::max(box.max, triangles[i].box.max);
            centroid_box.min = glm::min(centroid_box.min, triangles[i].centroid);
            centroid_box.max = glm::max(centroid_box.max, triangles[i].centroid);
        }
        bvh.nodes[range.node].box = box;

        if (range.count <= MAX_TRIANGLES_PER_LEAF) {
            bvh.nodes[range.node].first = range.first;
            bvh.nodes[range.node].count = range.count;
            continue;
        }

        const glm::vec3 spread = centroid_box.max - centroid_box.min;
        const int axis = (spread.x >= spread.y && spread.x >= spread.z) ? 0 : ((spread.y >= spread.z) ? 1 : 2);
        const uint32_t half = range.count / 2;
        std::nth_element(triangles.begin() + range.first, triangles.begin() + range.first + half, triangles.begin() + range.first + range.count,
                         [axis](const Triangle& a, const Triangle& b) { return a.centroid[axis] < b.centroid[axis]; });

        const uint32_t children = static_cast<uint32_t>(bvh.nodes.size());
        bvh.nodes.emplace_back();
        bvh.nodes.emplace_back();
        bvh.nodes[range.node].first = children;
        bvh.nodes[range.node].count = 0;
        work.push_back(Work{children, range.first, half});
        work.push_back(Work{children + 1, range.first + half, range.count - half});
    }

    // the triangles are now in the order of the leaves
    bvh.triangles.reserve(static_cast<size_t>(triangle_count) * 3);
    for (const Triangle& triangle : triangles) {
        bvh.triangles.insert(bvh.triangles.end(), triangle.indices.begin(), triangle.indices.end());
    }
    return bvh;
}

std::vector<uint8_t> makeCollisionBvhData(std::span<const std::pair<Name, CollisionBvh>> meshes)
{
    size_t size = ASSET_HEADER_SIZE + meshes.size() * sizeof(AssetMeshEntry);
    for (const auto& [mesh, bvh] : meshes) {
        size += bvh.positions.size() * sizeof(glm::vec3) + bvh.nodes.size() * sizeof(CollisionBvhNode) + alignUp(bvh.triangles.size() * sizeof(uint16_t));
    }

    std::vector<uint8_t> data(size); // (zeroes, for the padding)
    uint8_t* dest = data.data();
    const auto write = [&dest](const void* source, size_t source_size) {
        if (source_size > 0) {
            std::memcpy(dest, source, source_size);
        }
        dest += source_size;
    };

    const uint32_t header[2]{ASSET_VERSION, static_cast<uint32_t>(meshes.size())};
    static_assert(sizeof(header) == ASSET_HEADER_SIZE);
    write(header, sizeof(header));
    for (const auto& [mesh, bvh] : meshes) {
        const AssetMeshEntry entry{mesh.getHash(), static_cast<uint32_t>(bvh.positions.size()), static_cast<uint32_t>(bvh.triangles.size() / 3),
                                   static_cast<uint32_t>(bvh.nodes.size())};
        write(&entry, sizeof(entry));
    }
    for (const auto& [mesh, bvh] : meshes) {
        write(bvh.positions.data(), bvh.positions.size() * sizeof(glm::vec3));
        write(bvh.nodes.data(), bvh.nodes.size() * sizeof(CollisionBvhNode));
        write(bvh.triangles.data(), bvh.triangles.size() * sizeof(uint16_t));
        dest = data.data() + alignUp(static_cast<size_t>(dest - data.data()));
    }
    return data;
}

std::optional<ResourceCollisionBvh> ResourceCollisionBvh::create(std::span<const uint8_t> asset_data)
{
    if (asset_data.size() < ASSET_HEADER_SIZE) {
        return {};
    }
    uint32_t header[2]{};
    std::memcpy(header, asset_data.data(), sizeof(header));
    const size_t mesh_count = header[1];
    if (header[0] != ASSET_VERSION || mesh_count > (asset_data.size() - ASSET_HEADER_SIZE) / sizeof(AssetMeshEntry)) {
        return {};
    }

    ResourceCollisionBvh resource{};
    resource.meshes.reserve(mesh_count);
    size_t offset = ASSET_HEADER_SIZE + mesh_count * sizeof(AssetMeshEntry);
    for (size_t i = 0; i < mesh_count; ++i) {
        AssetMeshEntry entry{};
        std::memcpy(&entry, asset_data.data() + ASSET_HEADER_SIZE + i * sizeof(AssetMeshEntry), sizeof(entry));

        // (the counts are 32 bit, so these sizes can't overflow)
        const size_t positions_size = static_cast<size_t>(entry.position_count) * sizeof(glm::vec3);
        const size_t nodes_size = static_cast<size_t>(entry.node_count) * sizeof(CollisionBvhNode);
        const size_t triangles_size = alignUp(static_cast<size_t>(entry.triangle_count) * TRIANGLE_SIZE);
        if (positions_size + nodes_size + triangles_size > asset_data.size() - offset) {
            return {};
        }
        // a tree with triangles has somewhere to start, and the triangles have vertices
        if (entry.triangle_count > 0 && (entry.node_count == 0 || entry.position_count == 0)) {
            return {};
        }

        const uint8_t* const location = asset_data.data() + offset;
        CollisionBvhView view{};
        if (entry.triangle_count > 0) {
            view.positions = {reinterpret_cast<const glm::vec3*>(location), entry.position_count};
            view.nodes = {reinterpret_cast<const CollisionBvhNode*>(location + positions_size), entry.node_count};
            view.triangles = {reinterpret_cast<const uint16_t*>(location + positions_size + nodes_size), static_cast<size_t>(entry.triangle_count) * 3};
        }
        resource.meshes.try_emplace(Name(entry.mesh_id), view);
        offset += positions_size + nodes_size + triangles_size;
    }
    return resource;
}

std::optional<ResourceCollisionBvh> ResourceCollisionBvh::create(const Content& content_manager, Name name)
{
    const auto asset = content_manager.findAsset(name);
    if (asset.type != gcpak::GcpakAssetType::COLLISION_BVH) {
        return {};
    }
    return create(asset.data);
}

} // namespace gc
