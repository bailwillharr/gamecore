#pragma once

// Bakes the shadow map of any prefab, however it was made (from a glTF file, by a tool, or saved from a World).
//
// The prefab is read without loading it into a World: the baker only needs to know where its entities are, what they draw, and
// which way its directional light shines. Everything that the prefab draws casts a shadow, unless its entity is excluded by name.
// The shadow map becomes an asset named after the prefab ("level.prefab/shadowmap"), and the prefab gets a root entity named
// "shadow_map" with a ShadowMapComponent that uses it. A prefab that already has one (it was baked before) has it replaced.
//
// See shadow_baker.h for how the shadow map itself is made, and gc::ShadowMapComponent for how the engine uses it.

#include <cstdint>

#include <functional>
#include <span>
#include <string>
#include <vector>

#include <gamecore/gc_name.h>

#include "asset_compiler.h"
#include "shadow_baker.h"

// Finds the mesh, material and texture assets that the prefab refers to. Returns null if there is no asset with that ID.
// The assets must stay where they are until the bake has finished.
using AssetLookup = std::function<const Asset*(gc::Name id)>;

struct PrefabShadowOptions {
    uint32_t resolution{SHADOW_MAP_DEFAULT_RESOLUTION};
    // Entities with one of these names don't cast shadows, and neither do the entities under them. For things that are drawn
    // but aren't really there, such as a sky.
    std::vector<gc::Name> excluded_entities{};
};

enum class PrefabShadowResult {
    BAKED,
    NO_LIGHT,   // the prefab has no directional light, so there are no shadows to bake. Nothing was changed
    NO_CASTERS, // nothing in the prefab casts a shadow (or none of its meshes could be found). Nothing was changed
    CORRUPT,    // the prefab's data couldn't be read. Nothing was changed
};

// Bakes the shadows that the prefab's meshes cast in its directional light (the last one, if it has several, as that is the one
// the engine draws). On success, prefab_data is replaced by the prefab with its ShadowMapComponent, and shadow_map_out is the
// SHADOW_MAP_R16 asset that it refers to. prefab_name is the name of the prefab's asset. Problems are reported through the log.
PrefabShadowResult bakePrefabShadows(const std::string& prefab_name, std::vector<uint8_t>& prefab_data, const AssetLookup& find_asset,
                                     const PrefabShadowOptions& options, Asset& shadow_map_out);
