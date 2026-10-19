#pragma once

#include <array>
#include <filesystem>
#include <istream>
#include <ostream>
#include <bit>
#include <vector>
#include <optional>
#include <string>
#include <span>
#include <system_error>

/*
 * The gcpak file format contains many game assets
 *
 * Version 1
 *
 * File format layout:
 *  -- HEADER
 *  -- ASSET DATA
 *  -- ASSET DATA
 *  -- ASSET DATA
 *  -- ...
 *  -- ASSET 1 INFO ENTRY (crc32 id, size, offset)
 *  -- ASSET 2 INFO ENTRY
 *  -- ASSET 3 INFO ENTRY
 *  -- ...
 *
 * Max size of an asset is 4 GiB.
 * Max number of assets is UINT32_MAX + 1
 * Max size of the gcpak file is very large (64-bit offsets)
 */

namespace gcpak {

/* The file format uses little-endian data so the native endianness must match as no conversion is done */
static_assert(std::endian::native == std::endian::little);

constexpr std::array<uint8_t, 6> GCPAK_VALID_IDENTIFIER = {'G', 'C', 'P', 'A', 'K', '\0'};
constexpr uint16_t GCPAK_CURRENT_VERSION = 1;

struct GcpakHeader {
    std::array<std::uint8_t, 6> format_identifier; // null-terminated "GCPAK"
    uint16_t format_version;                       // currently 1
    uint32_t num_entries;

    void serialize(std::ostream& s) const
    {
        s.write(reinterpret_cast<const char*>(format_identifier.data()), format_identifier.size());
        s.write(reinterpret_cast<const char*>(&format_version), sizeof(uint16_t));
        s.write(reinterpret_cast<const char*>(&num_entries), sizeof(uint32_t));
    }

    static GcpakHeader deserialize(std::istream& s)
    {
        GcpakHeader header{};
        s.read(reinterpret_cast<char*>(header.format_identifier.data()), header.format_identifier.size());
        s.read(reinterpret_cast<char*>(&header.format_version), sizeof(uint16_t));
        s.read(reinterpret_cast<char*>(&header.num_entries), sizeof(uint32_t));
        return header;
    }

    static consteval size_t getSerializedSize() { return sizeof(format_identifier) + sizeof(format_version) + sizeof(num_entries); }
};

enum class GcpakAssetType : std::uint32_t {
    INVALID = 0,
    SPIRV_SHADER = 1,                           // passed directly into VkShaderModuleCreateInfo
    TEXTURE_R8G8B8A8 = 2,                       // first 4 bytes is width, second 4 bytes is height, remaining data is just R8G8B8A8.
                                                // The values are linear: for data such as normal maps, roughness and metallic
    MESH_POS12_NORM12_TANG16_UV8_INDEXED16 = 3, // first 2 bytes is vertex count, followed by vertices, followed by 16 bit indices
    PREFAB = 4,                                 // See gcpak_prefab.h
    MATERIAL = 5, // three asset IDs (uint32_t): base color texture, occlusion-roughness-metallic texture, normal texture. Zero means none.
                  // Then six floats, used in place of the textures that are none: base color (linear RGBA), roughness, metallic.
                  // Then the blend mode (uint32_t: 0 opaque, 1 alpha test, 2 alpha blend) and the alpha cutoff (float) for alpha test.
                  // Then the emissive texture's asset ID (uint32_t) and three floats that it is multiplied by (or that are the emission,
                  // if the ID is zero).
                  // Older files end after the first three asset IDs, after the six floats, or after the alpha cutoff.
    TEXTURE_R8G8B8A8_SRGB = 6, // same layout as TEXTURE_R8G8B8A8, but the RGB values are sRGB encoded: for colors (alpha is always linear)
    SHADOW_MAP_R16 = 7, // first 4 bytes is width, second 4 bytes is height, then one uint16_t per texel: the depth of the surface nearest
                        // to the light (0 is nearest, 65535 is farthest, or nothing). See gc::ShadowMapComponent
    COLLISION_BVH = 8,  // The triangles of several meshes, each sorted into a tree of bounding boxes for collision detection.
                        // Two uint32_t: the version (1) and the number of meshes. Then four uint32_t for each mesh: the mesh's asset ID,
                        // and its number of positions, triangles and nodes. Then for each mesh: its positions (three floats each), its
                        // nodes (six floats, the minimum and maximum of a box, then two uint32_t: see gc::CollisionBvhNode) and its
                        // triangles (three uint16_t each, indices into the positions), padded to a multiple of 4 bytes.
                        // See gc::CollisionBvhComponent
    CUBE_MAP_E5B9G9R9 = 9, // A cube map with high dynamic range, such as a sky. A header of 32 bytes: the width and height of a face
                           // and the number of mip levels (two uint32_t), then five floats that describe a sky (the direction of its
                           // sun, the illuminance of the sky without the sun, and of the sun: see gc::CubeMapInfo), then a uint32_t
                           // that is zero. Then the texels of each mip level, largest first: its six faces in the order +X -X +Y -Y
                           // +Z -Z, laid out as Vulkan's cube maps are, one uint32_t per texel in the format
                           // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32. Each level is half the size of the one before.
                           // See gc::SkyboxComponent
    TERRAIN = 10, // A height field, cut into tiles at several levels of detail for streaming. A header of 64 bytes: ten uint32_t
                  // (the version (1), the number of quads along a tile's side, the number of levels, the number of nodes at the top
                  // level along x and along y, the number of nodes, and the first and last+1 sample of the source data along x
                  // and along y), then five floats (the distance between samples, the height of a sample of 0, the height
                  // a step of 1 adds, and the lowest and highest heights), then a uint32_t that is zero. Then the nodes of a
                  // quadtree, 32 bytes each: the lowest
                  // and highest heights under the node, the depth of its skirts (all floats), its four children (uint32_t indices
                  // into the nodes, 0xffffffff for none) and a uint32_t of flags. The top level's nodes come first. Then each
                  // node's tile, in the same order: (quads + 1)^2 samples, rows first, one uint32_t each: the height in the low
                  // 16 bits, then the normal's x and y (int8_t each, of 127). See gc::ResourceTerrain
};

struct GcpakAssetEntry {
    uint64_t offset; // absolute positition of start of asset data in the file
    uint32_t crc32_id;
    GcpakAssetType asset_type;
    uint32_t size; // size of data in file

    void serialize(std::ostream& s) const
    {
        s.write(reinterpret_cast<const char*>(&offset), sizeof(uint64_t));
        s.write(reinterpret_cast<const char*>(&crc32_id), sizeof(uint32_t));
        s.write(reinterpret_cast<const char*>(&asset_type), sizeof(GcpakAssetType));
        s.write(reinterpret_cast<const char*>(&size), sizeof(uint32_t));
    }

    static GcpakAssetEntry deserialize(std::istream& s)
    {
        GcpakAssetEntry header{};
        s.read(reinterpret_cast<char*>(&header.offset), sizeof(uint64_t));
        s.read(reinterpret_cast<char*>(&header.crc32_id), sizeof(uint32_t));
        s.read(reinterpret_cast<char*>(&header.asset_type), sizeof(GcpakAssetType));
        s.read(reinterpret_cast<char*>(&header.size), sizeof(uint32_t));
        return header;
    }

    static consteval size_t getSerializedSize() { return sizeof(offset) + sizeof(crc32_id) + sizeof(asset_type) + sizeof(size); }
};

class GcpakCreator {
public:
    struct Asset {
        std::string name;
        uint32_t hash; // only used if name is empty
        std::vector<uint8_t> data;
        GcpakAssetType type;
    };

private:
    std::vector<Asset> m_assets{};

public:
    GcpakCreator() = default;

    std::optional<std::string> getError() const;

    std::span<const GcpakCreator::Asset> getAssets() const;

    void addAsset(const Asset& asset);

    /* Also saves a .txt file containing hashes, returns false if there was an IO error. */
    bool saveFile(const std::filesystem::path& path);

    // returns true on success
    // returns false and set ec on failure
    bool loadFile(const std::filesystem::path& existing_file, std::error_code& ec);

    void clear();
};

} // namespace gcpak
