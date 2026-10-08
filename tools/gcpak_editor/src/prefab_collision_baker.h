#pragma once

// Bakes the collision trees of any prefab, however it was made (from a glTF file, by a tool, or saved from a World).
//
// The engine collides with the triangles of a collider's mesh through a tree of bounding boxes (see gc_collision_bvh.h). Building
// those trees takes time, so it is done here rather than when the prefab is loaded: the tree of every mesh that one of the
// prefab's ColliderComponents uses is built and put into one asset, named after the prefab ("level.prefab/bvh"). The prefab gets
// a root entity named "collision_bvh" with a CollisionBvhComponent that uses it. A prefab that already has one (it was baked
// before) has it replaced.
//
// The trees are in the space of their meshes, so nothing here depends on where the prefab's entities are, or on where the prefab
// is put in the world. Baking again is only needed if the prefab's meshes change, or if it gets colliders with other meshes.

#include <cstdint>

#include <string>
#include <vector>

#include "asset_compiler.h"
#include "prefab_shadow_baker.h" // AssetLookup

enum class PrefabCollisionResult {
    BAKED,
    NO_COLLIDERS, // the prefab has no colliders (or none of their meshes could be found). Nothing was changed
    CORRUPT,      // the prefab's data couldn't be read. Nothing was changed
};

// On success, prefab_data is replaced by the prefab with its CollisionBvhComponent, and bvh_out is the COLLISION_BVH asset that
// it refers to. prefab_name is the name of the prefab's asset. find_asset finds the prefab's meshes. A mesh that isn't found is
// left out, with a warning: the engine builds its tree when the prefab is loaded. Problems are reported through the log.
PrefabCollisionResult bakePrefabCollision(const std::string& prefab_name, std::vector<uint8_t>& prefab_data, const AssetLookup& find_asset, Asset& bvh_out);
