#pragma once

#include <cstdint>

#include <span>

#include <gamecore/gc_resources.h>

gc::ResourceMesh genOBJMesh(std::span<const uint8_t> file_data);
gc::ResourceMesh genCuboidMesh(float tiling = 1.0f, bool wind_inside = false);
gc::ResourceMesh genPlaneMesh(float tiling = 1.0f);

// generates detail*detail triangles
gc::ResourceMesh genSphereMesh(int detail, bool flip_normals = false);
