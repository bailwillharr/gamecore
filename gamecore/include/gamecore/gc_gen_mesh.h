#pragma once

#include <cstdint>

#include <span>

#include "gamecore/gc_resources.h"

namespace gc {

ResourceMesh genOBJMesh(std::span<const uint8_t> file_data);
ResourceMesh genCubeMesh(float tiling = 1.0f, bool wind_inside = false);
ResourceMesh genPlaneMesh(float tiling_x = 1.0f, float tiling_y = 1.0f);

// generates detail*detail triangles
ResourceMesh genSphereMesh(int detail, bool flip_normals = false);

} // namespace gc
