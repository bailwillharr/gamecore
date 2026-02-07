#pragma once

#include <cstdint>

#include <span>

#include <gamecore/gc_resources.h>

gc::ResourceMesh genOBJMesh(std::span<const uint8_t> file_data);
gc::ResourceMesh genCuboidMesh(float x, float y, float z, float tiling = 1.0f, bool wind_inside = false);
gc::ResourceMesh genPlaneMesh(float tiling = 1.0f);
gc::ResourceMesh genSphereMesh(float r, int detail, bool flip_normals = false);
