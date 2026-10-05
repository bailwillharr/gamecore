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

    static std::optional<ResourceMesh> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.data.empty() || asset.type != gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16) {
            return {};
        }

        GC_ASSERT(asset.data.size() > sizeof(uint16_t));

        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset.data.data(), sizeof(uint16_t));

        const uint8_t* const vertices_location = asset.data.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset.data.data() + asset.data.size() - indices_location) / sizeof(uint16_t);

        GC_ASSERT(asset.data.size() == sizeof(uint16_t) + vertex_count * sizeof(MeshVertex) + index_count * sizeof(uint16_t));

        const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
        const auto vertices_end = vertices_begin + vertex_count;
        const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
        const auto indices_end = indices_begin + index_count;

        const std::span<const MeshVertex> vertices(vertices_begin, vertices_end);
        const std::span<const uint16_t> indices(indices_begin, indices_end);

        return ResourceMesh(vertices, indices);
    }
};

} // namespace gc
