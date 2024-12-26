#pragma once

#include <cstdint>

#include <string_view>
#include <string>
#include <format>
#include <unordered_map>
#include <filesystem>

#include "gamecore/gc_crc_table.h"

namespace gc {

/* uses crc_table (gc_crc_table.h) to generate a unique 32-bit hash */
inline constexpr std::uint32_t crc32(std::string_view id)
{
    uint32_t crc = 0xffffffffu;
    for (char c : id) crc = (crc >> 8) ^ crc_table[(crc ^ c) & 0xff];
    return crc ^ 0xffffffff;
}

/* Get the compile-time hash of the Asset ID. */
/* Use AssetIDRuntime() for runtime equivalent. */
inline consteval std::uint32_t assetID(std::string_view id) { return crc32(id); }

/* get the runtime hash of the Asset ID */
inline std::uint32_t assetIDRuntime(std::string_view id) { return crc32(id); }

// does nothing if GC_LOOKUP_ASSET_IDS isn't defined
// file_path should be the .txt file found with the .gcpak file of the same name
void loadAssetIDTable(const std::filesystem::path& file_path);

std::string nameFromID(std::uint32_t id);

} // namespace gc