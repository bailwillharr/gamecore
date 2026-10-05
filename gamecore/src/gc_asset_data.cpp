#include "gamecore/gc_asset_data.h"

#include <cstring>

#include "gamecore/gc_assert.h"

namespace gc {

std::vector<uint8_t> makeTextureData(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
{
    std::vector<uint8_t> data(2 * sizeof(uint32_t) + rgba.size());
    std::memcpy(data.data(), &width, sizeof(uint32_t));
    std::memcpy(data.data() + sizeof(uint32_t), &height, sizeof(uint32_t));
    std::memcpy(data.data() + 2 * sizeof(uint32_t), rgba.data(), rgba.size());
    return data;
}

std::vector<uint8_t> makeMeshData(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices)
{
    GC_ASSERT(vertices.size() <= UINT16_MAX);
    const uint16_t num_vertices = static_cast<uint16_t>(vertices.size());
    std::vector<uint8_t> data(sizeof(uint16_t) + vertices.size_bytes() + indices.size_bytes());
    std::memcpy(data.data(), &num_vertices, sizeof(uint16_t));
    std::memcpy(data.data() + sizeof(uint16_t), vertices.data(), vertices.size_bytes());
    std::memcpy(data.data() + sizeof(uint16_t) + vertices.size_bytes(), indices.data(), indices.size_bytes());
    return data;
}

std::vector<uint8_t> makeMaterialData(const ResourceMaterial& material)
{
    const uint32_t ids[3]{material.base_color_texture.getHash(), material.orm_texture.getHash(), material.normal_texture.getHash()};
    const float constants[6]{material.base_color.r, material.base_color.g, material.base_color.b, material.base_color.a, material.roughness, material.metallic};
    const uint32_t blend_mode = static_cast<uint32_t>(material.blend_mode);
    static_assert(sizeof(ids) == ResourceMaterial::TEXTURES_SIZE && sizeof(constants) == ResourceMaterial::CONSTANTS_SIZE);
    static_assert(sizeof(blend_mode) + sizeof(material.alpha_cutoff) == ResourceMaterial::BLEND_SIZE);
    const uint32_t emissive_id = material.emissive_texture.getHash();
    const float emissive[3]{material.emissive.r, material.emissive.g, material.emissive.b};
    static_assert(sizeof(emissive_id) + sizeof(emissive) == ResourceMaterial::EMISSIVE_SIZE);
    std::vector<uint8_t> data(sizeof(ids) + sizeof(constants) + ResourceMaterial::BLEND_SIZE + ResourceMaterial::EMISSIVE_SIZE);
    uint8_t* dest = data.data();
    std::memcpy(dest, ids, sizeof(ids));
    dest += sizeof(ids);
    std::memcpy(dest, constants, sizeof(constants));
    dest += sizeof(constants);
    std::memcpy(dest, &blend_mode, sizeof(blend_mode));
    dest += sizeof(blend_mode);
    std::memcpy(dest, &material.alpha_cutoff, sizeof(material.alpha_cutoff));
    dest += sizeof(material.alpha_cutoff);
    std::memcpy(dest, &emissive_id, sizeof(emissive_id));
    dest += sizeof(emissive_id);
    std::memcpy(dest, emissive, sizeof(emissive));
    return data;
}

} // namespace gc
