#pragma once

// Makes the data of a TERRAIN asset from a grid of heights. See gc_terrain.h for what a terrain is.
// Used by gcpak_editor (--bake-terrain), and by tests that make small terrains of their own.

#include <cstdint>

#include <span>
#include <string>
#include <vector>

namespace gc {

struct TerrainBuildInput {
    uint32_t width{};  // samples along x
    uint32_t height{}; // samples along y
    std::span<const float> heights{}; // width * height, rows first, from y = 0. NaN (or infinity) where there is no data
    float sample_spacing{1.0f};       // metres between samples
    uint32_t tile_quads{64};          // grid cells along a tile's side: a power of two, 4 or more
    // Where there is no data, only the tiles of this level and the levels above it are stored. 0 stores every tile. (A level that is
    // too high for the terrain means the top level.)
    uint32_t fill_level{3};
};

struct TerrainBuildInfo {
    uint32_t level_count{};
    uint32_t root_count_x{};
    uint32_t root_count_y{};
    uint32_t node_count{};
    std::vector<uint32_t> nodes_per_level{}; // from level 0
    float min_height{};
    float max_height{};
    float height_step{}; // the precision of the stored heights, in metres
    uint32_t filled_samples{}; // samples of level 0 that had no data, and were filled in
};

// Returns the data of the asset, or nothing (with error_out set) if the input can't be made into a terrain.
std::vector<uint8_t> makeTerrainData(const TerrainBuildInput& input, TerrainBuildInfo* info_out, std::string* error_out);

} // namespace gc
