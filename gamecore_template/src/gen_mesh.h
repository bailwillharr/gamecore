#pragma once

namespace gc {
class RenderMesh;    // forward-dec
class RenderBackend; // forward-dec
} // namespace gc

gc::RenderMesh genCuboidMesh(gc::RenderBackend& render_backend, float x, float y, float z, float tiling = 1.0f, bool wind_inside = false);
gc::RenderMesh genSphereMesh(gc::RenderBackend& render_backend, float r, int detail, bool wind_inside = false, bool flip_normals = false);