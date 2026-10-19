#pragma once

// Turns a grid of heights (a digital terrain model) into a TERRAIN asset (see gc_terrain.h). The heights are read from:
//
//   .IMG   a PDS3 image, as NASA's Planetary Data System publishes digital terrain models (HiRISE's DTEEC_*.IMG files, for one).
//          The label at the start of the file says how big the grid is, where its samples are, which value means "no data"
//          and how far apart the samples are (MAP_SCALE). Only 32 bit little endian floats (PC_REAL) are read. The first line
//          is the north edge, and becomes the terrain's +y edge.
//   .pfm   a portable float map with one channel. Its rows go from the bottom up. NaNs are missing data. It doesn't say how far
//          apart its samples are: give --spacing.
//
// Where there is no data (around the edges of a scan, usually), the gaps are filled with a smooth surface, at less detail.

#include <cstdint>

#include <filesystem>
#include <string>
#include <vector>

#include "asset_compiler.h"

struct TerrainCompileOptions {
    std::string name{};       // of the asset. Empty: the file's name with ".terrain" after it, e.g. "fissure.IMG.terrain"
    float spacing{};          // metres between samples. Zero: what the file says, or 1 if it doesn't say
    uint32_t tile_quads{64};  // see gc::TerrainBuildInput
    uint32_t fill_level{3};
};

// Appends the asset to assets_out. Returns false, having added nothing, if the file couldn't be read.
bool compileTerrain(const std::filesystem::path& path, const TerrainCompileOptions& options, std::vector<Asset>& assets_out);
