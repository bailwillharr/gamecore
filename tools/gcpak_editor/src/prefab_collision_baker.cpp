#include "prefab_collision_baker.h"

#include <format>
#include <optional>
#include <unordered_set>
#include <utility>

#include <gclog/gclog.h>

#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_collision_bvh.h>
#include <gamecore/gc_collision_bvh_component.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_stopwatch.h>

#include "prefab_file.h"

namespace {

constexpr gc::Name COLLISION_BVH_ENTITY_NAME = gc::Name::createConstexpr("collision_bvh");

} // namespace

PrefabCollisionResult bakePrefabCollision(const std::string& prefab_name, std::vector<uint8_t>& prefab_data, const AssetLookup& find_asset, Asset& bvh_out)
{
    const std::optional<std::vector<PrefabEntity>> entities = readPrefab(prefab_data);
    if (!entities) {
        GC_ERROR("{}: the prefab is corrupt", prefab_name);
        return PrefabCollisionResult::CORRUPT;
    }

    gc::Stopwatch stopwatch{};

    // Each mesh once, in the order that the prefab uses them
    std::vector<std::pair<gc::Name, gc::CollisionBvh>> meshes{};
    std::unordered_set<gc::Name> seen{};
    uint32_t collider_count = 0;
    uint32_t missing_meshes = 0;
    size_t triangle_count = 0;
    for (const PrefabEntity& entity : *entities) {
        for (const PrefabComponent& component : entity.components) {
            const auto collider = readComponent<gc::ColliderComponent>(component);
            if (!collider || collider->m_mesh.empty()) {
                continue;
            }
            ++collider_count;
            if (!seen.insert(collider->m_mesh).second) {
                continue;
            }
            const Asset* const asset = find_asset(collider->m_mesh);
            const std::optional<gc::ResourceMesh> mesh =
                (asset && asset->type == gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16) ? gc::ResourceMesh::create(asset->data) : std::nullopt;
            if (!mesh) {
                // e.g. a mesh that the game generates when it starts
                GC_WARN("{}: the mesh {} of entity '{}' wasn't found, so its collision tree will be built when it is loaded", prefab_name, collider->m_mesh,
                        entity.transform.name);
                ++missing_meshes;
                continue;
            }
            // (a mesh without triangles is kept, with an empty tree, so that the engine doesn't go looking for the mesh)
            meshes.emplace_back(collider->m_mesh, gc::buildCollisionBvh(mesh->vertices.get(), mesh->indices.get()));
            triangle_count += meshes.back().second.triangles.size() / 3;
        }
    }

    if (meshes.empty()) {
        if (collider_count == 0) {
            GC_INFO("{}: has no colliders, so there are no collision trees to bake", prefab_name);
        }
        else {
            GC_WARN("{}: none of the meshes of its colliders were found", prefab_name);
        }
        return PrefabCollisionResult::NO_COLLIDERS;
    }

    const std::string asset_name = std::format("{}/bvh", prefab_name);
    bvh_out = makeAsset(asset_name, gc::makeCollisionBvhData(meshes), gcpak::GcpakAssetType::COLLISION_BVH);

    prefab_data = writePrefabWithBakedComponent(*entities, COLLISION_BVH_ENTITY_NAME, gc::CollisionBvhComponent{}.setBvh(gc::Name(asset_name)));

    GC_INFO("{}: baked the collision trees of {} meshes ({} triangles, used by {} colliders) in {}", prefab_name, meshes.size(), triangle_count,
            collider_count, stopwatch);
    if (missing_meshes > 0) {
        GC_WARN("{}: {} meshes weren't found. Their collision trees will be built when they are loaded", prefab_name, missing_meshes);
    }
    return PrefabCollisionResult::BAKED;
}
