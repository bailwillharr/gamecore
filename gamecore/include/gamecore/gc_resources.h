#pragma once

#include <cstdlib>
#include <cstring>

#include <array>
#include <span>
#include <optional>
#include <variant>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gcpak/gcpak.h>

#include <gctemplates/gct_maybe_owning.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_material_blend_mode.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_mesh_vertex.h"

// NB
// Resources don't need to be serialisable, but they should be copyable and loadable from disk.

namespace gc {

struct ResourceTexture {
    gct::MaybeOwning<uint8_t> data;
    bool srgb;

    ResourceTexture() = default;

    ResourceTexture(std::vector<uint8_t> data, bool srgb) : data(std::move(data)), srgb(srgb) {}

    ResourceTexture(std::span<const uint8_t> data, bool srgb) : data(data), srgb(srgb) {}

    static std::optional<ResourceTexture> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        // the type of the asset says whether the texture holds colors (sRGB) or data (linear)
        const bool srgb = (asset.type == gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB);
        if (asset.data.empty() || (!srgb && asset.type != gcpak::GcpakAssetType::TEXTURE_R8G8B8A8)) {
            return {};
        }

        return ResourceTexture(asset.data, srgb);
    }
};

// The depths of a baked shadow map. See ShadowMapComponent
struct ResourceShadowMap {
    gct::MaybeOwning<uint8_t> data; // as in the asset: width, height, then 16 bit depths

    ResourceShadowMap() = default;

    explicit ResourceShadowMap(std::span<const uint8_t> data) : data(data) {}

    static std::optional<ResourceShadowMap> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.type != gcpak::GcpakAssetType::SHADOW_MAP_R16 || asset.data.size() <= 2 * sizeof(uint32_t)) {
            return {};
        }
        uint32_t width{}, height{};
        std::memcpy(&width, asset.data.data(), sizeof(uint32_t));
        std::memcpy(&height, asset.data.data() + sizeof(uint32_t), sizeof(uint32_t));
        if (asset.data.size() != 2 * sizeof(uint32_t) + static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint16_t)) {
            return {};
        }
        return ResourceShadowMap(asset.data);
    }
};

// What a cube map asset says about itself. See GcpakAssetType::CUBE_MAP_E5B9G9R9
struct CubeMapInfo {
    uint32_t face_size{};
    uint32_t mip_count{};
    // The rest is only set for a cube map that is a sky (and not for the irradiance and reflections that go with it). The
    // illuminances are in the units of the texels: what a texel of 1 is worth is up to whoever draws the sky.
    glm::vec3 sun_direction{0.0f, 0.0f, 0.0f}; // towards the sun, in the cube map's space. Zero if the sky has no sun
    float sky_illuminance{};                   // of a surface facing up (+Z), from the whole sky without its sun
    float sun_illuminance{};                   // of a surface facing the sun, from the sun alone
};

// A cube map with high dynamic range. See SkyboxComponent
struct ResourceCubeMap {
    gct::MaybeOwning<uint8_t> data; // as in the asset
    CubeMapInfo info{};

    static constexpr size_t HEADER_SIZE = 8 * sizeof(uint32_t);

    // how many texels a cube map has, in all of its faces and mip levels
    static size_t getTexelCount(uint32_t face_size, uint32_t mip_count)
    {
        size_t count = 0;
        for (uint32_t level = 0; level < mip_count; ++level) {
            const size_t level_size = face_size >> level;
            count += 6 * level_size * level_size;
        }
        return count;
    }

    ResourceCubeMap() = default;

    // Empty if the data isn't a whole cube map
    static std::optional<ResourceCubeMap> create(std::span<const uint8_t> asset_data)
    {
        if (asset_data.size() <= HEADER_SIZE) {
            return {};
        }
        std::array<uint32_t, 2> sizes{};
        std::array<float, 5> sky{};
        std::memcpy(sizes.data(), asset_data.data(), sizeof(sizes));
        std::memcpy(sky.data(), asset_data.data() + sizeof(sizes), sizeof(sky));

        ResourceCubeMap cube_map{};
        cube_map.info.face_size = sizes[0];
        cube_map.info.mip_count = sizes[1];
        cube_map.info.sun_direction = glm::vec3{sky[0], sky[1], sky[2]};
        cube_map.info.sky_illuminance = sky[3];
        cube_map.info.sun_illuminance = sky[4];
        // (every level has to have at least a texel, and the sizes are small enough that the sum below can't overflow)
        if (sizes[0] == 0 || sizes[0] > 32768 || sizes[1] == 0 || sizes[1] > 16 || (sizes[0] >> (sizes[1] - 1)) == 0 ||
            asset_data.size() != HEADER_SIZE + getTexelCount(sizes[0], sizes[1]) * sizeof(uint32_t)) {
            return {};
        }
        cube_map.data = asset_data;
        return cube_map;
    }

    static std::optional<ResourceCubeMap> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.type != gcpak::GcpakAssetType::CUBE_MAP_E5B9G9R9) {
            return {};
        }
        return create(asset.data);
    }
};

struct ResourceMaterial {
    // Any of these can be empty. A material is drawn by a shader that only samples the textures it has.
    Name base_color_texture;
    Name orm_texture;
    Name normal_texture;
    Name emissive_texture;

    // Used in place of the textures that the material doesn't have. Without a normal texture, the mesh's normals are used.
    glm::vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f}; // linear
    float roughness = 0.5f;
    float metallic = 0.0f;

    // What the alpha (of the base color texture, or of base_color if there is no texture) does
    MaterialBlendMode blend_mode = MaterialBlendMode::NONE;
    float alpha_cutoff = 0.5f; // only for MaterialBlendMode::ALPHA_TEST

    // The light that the material gives off itself: the emissive texture multiplied by this, or just this if there is no texture.
    // It is added to what the material reflects, after the camera's exposure, so it always looks the same: 1 is about as bright
    // as a white surface in full light, however bright or dark the world is. It doesn't light anything else.
    glm::vec3 emissive{0.0f, 0.0f, 0.0f};

    // See GcpakAssetType::MATERIAL
    static constexpr size_t TEXTURES_SIZE = 3 * sizeof(uint32_t);
    static constexpr size_t CONSTANTS_SIZE = 6 * sizeof(float);
    static constexpr size_t BLEND_SIZE = sizeof(uint32_t) + sizeof(float);
    static constexpr size_t EMISSIVE_SIZE = sizeof(uint32_t) + 3 * sizeof(float);

    // Each part was added to the format after the one before it, and older assets end where the format ended when they were made
    static bool isValidAssetSize(size_t size)
    {
        return size == TEXTURES_SIZE || size == TEXTURES_SIZE + CONSTANTS_SIZE || size == TEXTURES_SIZE + CONSTANTS_SIZE + BLEND_SIZE ||
               size == TEXTURES_SIZE + CONSTANTS_SIZE + BLEND_SIZE + EMISSIVE_SIZE;
    }

    static std::optional<ResourceMaterial> create(std::span<const uint8_t> asset_data)
    {
        if (!isValidAssetSize(asset_data.size())) {
            return {};
        }

        std::array<uint32_t, 3> texture_ids{};
        std::memcpy(texture_ids.data(), asset_data.data(), TEXTURES_SIZE);

        ResourceMaterial material{};
        material.base_color_texture = Name(texture_ids[0]);
        material.orm_texture = Name(texture_ids[1]);
        material.normal_texture = Name(texture_ids[2]);
        if (asset_data.size() >= TEXTURES_SIZE + CONSTANTS_SIZE) {
            std::array<float, 6> constants{};
            std::memcpy(constants.data(), asset_data.data() + TEXTURES_SIZE, CONSTANTS_SIZE);
            material.base_color = glm::vec4{constants[0], constants[1], constants[2], constants[3]};
            material.roughness = constants[4];
            material.metallic = constants[5];
        }
        if (asset_data.size() == TEXTURES_SIZE + CONSTANTS_SIZE + BLEND_SIZE + EMISSIVE_SIZE) {
            const uint8_t* const emissive_data = asset_data.data() + TEXTURES_SIZE + CONSTANTS_SIZE + BLEND_SIZE;
            uint32_t emissive_texture{};
            std::array<float, 3> emissive{};
            std::memcpy(&emissive_texture, emissive_data, sizeof(uint32_t));
            std::memcpy(emissive.data(), emissive_data + sizeof(uint32_t), 3 * sizeof(float));
            material.emissive_texture = Name(emissive_texture);
            material.emissive = glm::vec3{emissive[0], emissive[1], emissive[2]};
        }
        if (asset_data.size() >= TEXTURES_SIZE + CONSTANTS_SIZE + BLEND_SIZE) {
            uint32_t blend_mode{};
            std::memcpy(&blend_mode, asset_data.data() + TEXTURES_SIZE + CONSTANTS_SIZE, sizeof(uint32_t));
            std::memcpy(&material.alpha_cutoff, asset_data.data() + TEXTURES_SIZE + CONSTANTS_SIZE + sizeof(uint32_t), sizeof(float));
            material.blend_mode = (blend_mode < MATERIAL_BLEND_MODE_COUNT) ? static_cast<MaterialBlendMode>(blend_mode) : MaterialBlendMode::NONE;
        }
        return material;
    }

    static std::optional<ResourceMaterial> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.type != gcpak::GcpakAssetType::MATERIAL) {
            return {};
        }
        return create(asset.data);
    }
};

struct ResourceMesh {

    gct::MaybeOwning<MeshVertex> vertices;
    gct::MaybeOwning<uint16_t> indices;

    ResourceMesh() = default;

    ResourceMesh(std::vector<MeshVertex> vertices, std::vector<uint16_t> indices) : vertices(std::move(vertices)), indices(std::move(indices)) {}

    ResourceMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices) : vertices(vertices), indices(indices) {}

    // The data of a mesh asset. The vertices and indices are views of it. Empty if the data is too small for its vertices
    static std::optional<ResourceMesh> create(std::span<const uint8_t> asset_data)
    {
        if (asset_data.size() <= sizeof(uint16_t)) {
            return {};
        }

        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset_data.data(), sizeof(uint16_t));
        if (asset_data.size() < sizeof(uint16_t) + vertex_count * sizeof(MeshVertex)) {
            return {};
        }

        const uint8_t* const vertices_location = asset_data.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset_data.data() + asset_data.size() - indices_location) / sizeof(uint16_t);

        const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
        const auto vertices_end = vertices_begin + vertex_count;
        const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
        const auto indices_end = indices_begin + index_count;

        const std::span<const MeshVertex> vertices(vertices_begin, vertices_end);
        const std::span<const uint16_t> indices(indices_begin, indices_end);

        return ResourceMesh(vertices, indices);
    }

    static std::optional<ResourceMesh> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.data.empty() || asset.type != gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16) {
            return {};
        }

        auto mesh = create(asset.data);
        GC_ASSERT(mesh.has_value());
        GC_ASSERT(asset.data.size() == sizeof(uint16_t) + mesh->vertices.get().size_bytes() + mesh->indices.get().size_bytes());
        return mesh;
    }
};

} // namespace gc
