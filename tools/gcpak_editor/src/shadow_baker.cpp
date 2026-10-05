#include "shadow_baker.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include <algorithm>
#include <array>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <gamecore/gc_mesh_vertex.h>

namespace {

// Reads the triangles of a mesh asset. The asset's data isn't aligned, so everything is copied out of it.
class MeshReader {
    const uint8_t* m_vertices{};
    const uint8_t* m_indices{};
    size_t m_vertex_count{};
    size_t m_index_count{};

public:
    explicit MeshReader(std::span<const uint8_t> mesh_data)
    {
        if (mesh_data.size() < sizeof(uint16_t)) {
            return;
        }
        uint16_t vertex_count{};
        std::memcpy(&vertex_count, mesh_data.data(), sizeof(uint16_t));
        const size_t vertices_size = static_cast<size_t>(vertex_count) * sizeof(gc::MeshVertex);
        if (mesh_data.size() < sizeof(uint16_t) + vertices_size) {
            return;
        }
        m_vertices = mesh_data.data() + sizeof(uint16_t);
        m_indices = m_vertices + vertices_size;
        m_vertex_count = vertex_count;
        m_index_count = (mesh_data.size() - sizeof(uint16_t) - vertices_size) / sizeof(uint16_t);
    }

    size_t getVertexCount() const { return m_vertex_count; }
    size_t getTriangleCount() const { return m_index_count / 3; }

    glm::vec2 getUV(size_t vertex) const
    {
        glm::vec2 uv{};
        std::memcpy(&uv, m_vertices + vertex * sizeof(gc::MeshVertex) + offsetof(gc::MeshVertex, uv), sizeof(uv));
        return uv;
    }

    glm::vec3 getPosition(size_t vertex) const
    {
        glm::vec3 position{};
        std::memcpy(&position, m_vertices + vertex * sizeof(gc::MeshVertex) + offsetof(gc::MeshVertex, position), sizeof(position));
        return position;
    }

    // false if the triangle refers to a vertex that doesn't exist
    bool getTriangle(size_t triangle, std::array<size_t, 3>& vertices) const
    {
        std::array<uint16_t, 3> indices{};
        std::memcpy(indices.data(), m_indices + triangle * 3 * sizeof(uint16_t), sizeof(indices));
        for (int i = 0; i < 3; ++i) {
            if (indices[i] >= m_vertex_count) {
                return false;
            }
            vertices[i] = indices[i];
        }
        return true;
    }
};

// The alpha channel of a texture asset, looked up as the engine's sampler does: the texture repeats, and (0, 0) is the first texel.
// It reads the nearest texel of the full size texture, where the engine filters, so the edges of holes are a texel or so out.
class AlphaTexture {
    const uint8_t* m_rgba{};
    uint32_t m_width{};
    uint32_t m_height{};

public:
    explicit AlphaTexture(std::span<const uint8_t> texture_data)
    {
        if (texture_data.size() < 2 * sizeof(uint32_t)) {
            return;
        }
        uint32_t width{}, height{};
        std::memcpy(&width, texture_data.data(), sizeof(uint32_t));
        std::memcpy(&height, texture_data.data() + sizeof(uint32_t), sizeof(uint32_t));
        if (width == 0 || height == 0 || texture_data.size() != 2 * sizeof(uint32_t) + static_cast<size_t>(width) * height * 4) {
            return;
        }
        m_rgba = texture_data.data() + 2 * sizeof(uint32_t);
        m_width = width;
        m_height = height;
    }

    bool isValid() const { return m_rgba != nullptr; }

    float getAlpha(float u, float v) const
    {
        const float wrapped_u = u - std::floor(u);
        const float wrapped_v = v - std::floor(v);
        const uint32_t x = std::min(static_cast<uint32_t>(wrapped_u * static_cast<float>(m_width)), m_width - 1);
        const uint32_t y = std::min(static_cast<uint32_t>(wrapped_v * static_cast<float>(m_height)), m_height - 1);
        return static_cast<float>(m_rgba[(static_cast<size_t>(y) * m_width + x) * 4 + 3]) / 255.0f;
    }
};

} // namespace

std::optional<BakedShadowMap> bakeShadowMap(std::span<const ShadowCaster> casters, const glm::vec3& direction_to_light, uint32_t resolution)
{
    if (resolution == 0 || !(glm::dot(direction_to_light, direction_to_light) > 0.0f)) {
        return {};
    }

    // Light space: Z points towards the light, so the light looks down -Z, and X and Y span the shadow map.
    // The light is infinitely far away, so this is only a rotation.
    const glm::vec3 axis_z = glm::normalize(direction_to_light);
    const glm::vec3 up = (std::abs(axis_z.z) < 0.99f) ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 axis_x = glm::normalize(glm::cross(up, axis_z));
    const glm::vec3 axis_y = glm::cross(axis_z, axis_x);
    glm::mat4 to_light_space{1.0f};
    for (int column = 0; column < 3; ++column) {
        to_light_space[column][0] = axis_x[column];
        to_light_space[column][1] = axis_y[column];
        to_light_space[column][2] = axis_z[column];
    }

    // every vertex in light space
    std::vector<std::vector<glm::vec3>> caster_positions(casters.size());
    glm::vec3 bounds_min{std::numeric_limits<float>::max()};
    glm::vec3 bounds_max{std::numeric_limits<float>::lowest()};
    uint64_t triangle_count = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
        const MeshReader mesh(casters[i].mesh_data);
        const glm::mat4 matrix = to_light_space * casters[i].matrix;
        caster_positions[i].resize(mesh.getVertexCount());
        for (size_t v = 0; v < mesh.getVertexCount(); ++v) {
            const glm::vec3 position = glm::vec3(matrix * glm::vec4(mesh.getPosition(v), 1.0f));
            caster_positions[i][v] = position;
            bounds_min = glm::min(bounds_min, position);
            bounds_max = glm::max(bounds_max, position);
        }
        triangle_count += mesh.getTriangleCount();
    }
    if (triangle_count == 0 || !(bounds_max.x >= bounds_min.x)) {
        return {};
    }

    // A margin of a few texels around the edge, so that the shadows of things at the edge aren't cut off by the filtering.
    // The depth range gets a margin too, so that the nearest and farthest surfaces aren't exactly 0 and 1.
    const float margin = 4.0f / static_cast<float>(resolution);
    glm::vec3 extent = glm::max(bounds_max - bounds_min, glm::vec3{1.0e-3f});
    bounds_min -= extent * glm::vec3{margin, margin, 0.01f};
    bounds_max += extent * glm::vec3{margin, margin, 0.01f};
    extent = bounds_max - bounds_min;

    BakedShadowMap result{};
    result.resolution = resolution;
    result.texel_size = glm::max(extent.x, extent.y) / static_cast<float>(resolution);
    result.depth_range = extent.z;
    result.triangle_count = triangle_count;

    // light space to texture coordinates and depth. Depth is zero at the surface nearest to the light (the greatest Z)
    glm::mat4 to_shadow_map{1.0f};
    to_shadow_map[0][0] = 1.0f / extent.x;
    to_shadow_map[3][0] = -bounds_min.x / extent.x;
    to_shadow_map[1][1] = 1.0f / extent.y;
    to_shadow_map[3][1] = -bounds_min.y / extent.y;
    to_shadow_map[2][2] = -1.0f / extent.z;
    to_shadow_map[3][2] = bounds_max.z / extent.z;
    result.matrix = to_shadow_map * to_light_space;

    // Rasterise every triangle, whichever way it faces, keeping the nearest depth at each texel's centre.
    std::vector<float> depths(static_cast<size_t>(resolution) * resolution, 1.0f);
    const float size = static_cast<float>(resolution);
    const int last_texel = static_cast<int>(resolution) - 1;
    for (size_t i = 0; i < casters.size(); ++i) {
        const MeshReader mesh(casters[i].mesh_data);
        const std::vector<glm::vec3>& positions = caster_positions[i];

        // Does the caster have holes? If it has a texture, the alpha is tested at every texel of the shadow map that it covers.
        const AlphaTexture alpha_texture(casters[i].alpha_texture_data);
        const float alpha_cutoff = casters[i].alpha_cutoff;
        const bool test_alpha = alpha_cutoff > 0.0f && alpha_texture.isValid();
        if (alpha_cutoff > 0.0f && !alpha_texture.isValid() && casters[i].alpha < alpha_cutoff) {
            continue; // all of it is a hole
        }

        for (size_t t = 0; t < mesh.getTriangleCount(); ++t) {
            std::array<size_t, 3> vertices{};
            if (!mesh.getTriangle(t, vertices)) {
                continue;
            }
            std::array<glm::vec2, 3> uv{};
            if (test_alpha) {
                for (int v = 0; v < 3; ++v) {
                    uv[v] = mesh.getUV(vertices[v]);
                }
            }
            // x and y in texels, z in depth
            std::array<float, 3> x{}, y{}, z{};
            for (int v = 0; v < 3; ++v) {
                const glm::vec3& position = positions[vertices[v]];
                x[v] = (position.x - bounds_min.x) / extent.x * size;
                y[v] = (position.y - bounds_min.y) / extent.y * size;
                z[v] = (bounds_max.z - position.z) / extent.z;
            }

            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (!(std::abs(area) > 1.0e-12f)) {
                continue; // edge on to the light, or degenerate
            }
            const float inverse_area = 1.0f / area;

            // the texels whose centres can be inside the triangle
            const int min_x = std::max(static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}) - 0.5f)), 0);
            const int max_x = std::min(static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}) - 0.5f)), last_texel);
            const int min_y = std::max(static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}) - 0.5f)), 0);
            const int max_y = std::min(static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}) - 0.5f)), last_texel);

            for (int py = min_y; py <= max_y; ++py) {
                const float cy = static_cast<float>(py) + 0.5f;
                float* const row = depths.data() + static_cast<size_t>(py) * resolution;
                for (int px = min_x; px <= max_x; ++px) {
                    const float cx = static_cast<float>(px) + 0.5f;
                    // barycentric coordinates of the texel's centre
                    const float w0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) * inverse_area;
                    const float w1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) * inverse_area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;
                    }
                    const float depth = w0 * z[0] + w1 * z[1] + w2 * z[2];
                    if (depth < row[px]) {
                        if (test_alpha) {
                            // (texture coordinates vary linearly across the shadow map, as the light's view has no perspective)
                            const glm::vec2 point_uv = w0 * uv[0] + w1 * uv[1] + w2 * uv[2];
                            if (alpha_texture.getAlpha(point_uv.x, point_uv.y) < alpha_cutoff) {
                                continue;
                            }
                        }
                        row[px] = depth;
                    }
                }
            }
        }
    }

    result.depths.resize(depths.size());
    for (size_t i = 0; i < depths.size(); ++i) {
        result.depths[i] = static_cast<uint16_t>(std::lround(std::clamp(depths[i], 0.0f, 1.0f) * 65535.0f));
    }
    return result;
}

std::vector<uint8_t> makeShadowMapData(const BakedShadowMap& shadow_map)
{
    const uint32_t size[2]{shadow_map.resolution, shadow_map.resolution};
    const size_t depths_size = shadow_map.depths.size() * sizeof(uint16_t);
    std::vector<uint8_t> data(sizeof(size) + depths_size);
    std::memcpy(data.data(), size, sizeof(size));
    std::memcpy(data.data() + sizeof(size), shadow_map.depths.data(), depths_size);
    return data;
}
