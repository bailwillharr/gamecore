#pragma once

// Bakes the shadow map of a directional light: the depth, seen from the light, of the nearest surface in every direction.
// It is done on the CPU so that packaging from the command line doesn't need a renderer.
// See gc::ShadowMapComponent for how the engine uses the result.

#include <cstdint>

#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// One mesh that casts shadows
struct ShadowCaster {
    std::span<const uint8_t> mesh_data; // the data of a mesh asset (see makeMeshData())
    glm::mat4 matrix;                   // from the mesh's space to the space that the shadow map is baked in

    // For materials with holes in them (alpha tested): only the parts whose alpha is at least alpha_cutoff cast a shadow.
    // The alpha is read from the texture, which is the data of a texture asset (see makeTextureData()), with the mesh's texture
    // coordinates. Without a texture, the whole mesh has the alpha 'alpha'.
    // The defaults make everything cast a shadow.
    float alpha_cutoff{0.0f};
    float alpha{1.0f};
    std::span<const uint8_t> alpha_texture_data{};
};

struct BakedShadowMap {
    uint32_t resolution{};          // the shadow map is square
    std::vector<uint16_t> depths{}; // resolution * resolution, in rows. 0 is nearest to the light, 65535 is farthest (or nothing)
    glm::mat4 matrix{};             // from the space of the casters to texture coordinates (x, y: 0 to 1) and depth (z: 0 to 1)
    float texel_size{};             // the size of a texel in the space of the casters
    float depth_range{};            // the distance that depths 0 to 1 cover
    uint64_t triangle_count{};
};

constexpr uint32_t SHADOW_MAP_DEFAULT_RESOLUTION = 4096;

// direction_to_light points from the world towards the light. Empty if there is nothing to cast shadows.
std::optional<BakedShadowMap> bakeShadowMap(std::span<const ShadowCaster> casters, const glm::vec3& direction_to_light,
                                            uint32_t resolution = SHADOW_MAP_DEFAULT_RESOLUTION);

// The data of a SHADOW_MAP_R16 asset. See gcpak.h
std::vector<uint8_t> makeShadowMapData(const BakedShadowMap& shadow_map);
