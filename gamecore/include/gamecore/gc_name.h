#pragma once

#include <cstdint>

#include <string_view>
#include <string>
#include <filesystem>

#include "gamecore/gc_crc_table.h"

namespace gc {

using Name = uint32_t;

// does nothing if GC_LOOKUP_ASSET_IDS isn't defined
void addNameLookup(Name name, const std::string& str);

// does nothing if GC_LOOKUP_ASSET_IDS isn't defined
// file_path should be the .txt file found with the .gcpak file of the same name
void loadNameLookupTable(const std::filesystem::path& file_path);

// does nothing if GC_LOOKUP_ASSET_IDS isn't defined
void debugLogNameLookups();

// if GC_LOOKUP_ASSET_IDS isn't defined, it just returns the hexadecimal hash
std::string nameToStr(Name asset_id);

/* uses crc_table (gc_crc_table.h) to generate a unique 32-bit hash */
inline constexpr uint32_t crc32(std::string_view id)
{
    uint32_t crc = 0xffffffffu;
    for (char c : id) crc = (crc >> 8) ^ crc_table[(crc ^ c) & 0xff];
    return crc ^ 0xffffffff;
}

/* Convert a string to a hash of the string for more efficient storage and comparisons. */
#ifdef GC_LOOKUP_ASSET_IDS
inline Name strToNameRuntime(std::string_view str)
{
    const Name name = crc32(str);
    addNameLookup(name, std::string(str));
    return crc32(str);
}
inline Name strToName(std::string_view str) { return strToNameRuntime(str); }
#else
inline Name strToNameRuntime(std::string_view str) { return crc32(str); }
inline consteval Name strToName(std::string_view str) { return crc32(str); }
#endif

} // namespace gc
