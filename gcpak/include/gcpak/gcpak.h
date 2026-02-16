#pragma once

#include <array>
#include <filesystem>
#include <istream>
#include <ostream>
#include <bit>
#include <vector>
#include <optional>
#include <string>

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
    TEXTURE_R8G8B8A8 = 2,                       // first 4 bytes is width, second 4 bytes is height, remaining data is just R8G8B8A8
    MESH_POS12_NORM12_TANG16_UV8_INDEXED16 = 3, // first 2 bytes is vertex count, followed by vertices, followed by 16 bit indices
    PREFAB = 4, // See gcpak_prefab.h
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
    std::optional<std::string> m_existing_file_load_error{};

public:
    GcpakCreator() = default;

    GcpakCreator(const std::filesystem::path& existing_file);

    std::optional<std::string> getError() const;

    void addAsset(const Asset& asset);

    /* Also saves a .txt file containing hashes, returns false if there was an IO error. */
    bool saveFile(const std::filesystem::path& path);
};

} // namespace gcpak
