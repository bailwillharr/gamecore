#pragma once

// std::strings are expensive to compare and often require dynamic heap allocation for storage.
// When strings are only needed for unique identifiers, such as looking up assets or resources
// or giving names to entities, std::strings are overkill.
// The 'Name' class stores only the hash of the string.
// Names can be compared to other Names as different strings yield unique hashes.
// The constructor is constexpr so hashes can be computed at compile-time.
// For debugging, a look up table stores all hashes and their corresponding strings, accessed via Name::getString()
// In addition, the Name class has a specialization for std::hash that does nothing (it's already a hash), which speeds up unordered_map<Name, T> lookups.

#include <cstdint>

#include <string_view>
#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "gamecore/gc_crc_table.h"
#include "gamecore/gc_logger.h"

namespace gc {

class Name {
#ifdef GC_LOOKUP_ASSET_IDS
    inline static std::unordered_map<uint32_t, std::string> s_lut{};
#endif
    uint32_t m_hash;

public:
    constexpr Name() : m_hash(0) {}
    explicit constexpr Name(uint32_t hash) : m_hash(hash) {}
#ifdef GC_LOOKUP_ASSET_IDS
    explicit Name(std::string_view str) : m_hash(crc32(str))
    {
        // can't pass *this as object isn't constructed yet
        s_lut.emplace(m_hash, str);
    }
#else
    explicit constexpr Name(std::string_view str) : m_hash(crc32(str)) {}
#endif

    constexpr bool operator==(const Name& other) const noexcept { return m_hash == other.m_hash; }

    constexpr uint32_t getHash() const noexcept { return m_hash; }

    constexpr bool empty() const noexcept { return m_hash == 0; }

    std::string getString() const
    {
#ifdef GC_LOOKUP_ASSET_IDS
        if (s_lut.contains(m_hash)) {
            return s_lut.at(m_hash);
        }
        else {
            return std::format("{:#08x}", m_hash);
        }
#else
        return std::format("{:#08x}", m_hash);
#endif
    }
};

inline void loadNameLookupTable(const std::filesystem::path& file_path)
{
    std::ifstream file(file_path);
    if (!file) {
        GC_ERROR("Failed to open file: {}", file_path.filename().string());
        return;
    }
    std::string line{};
    file.seekg(0);
    while (std::getline(file, line)) {
        uint32_t hash;
        std::from_chars_result res = std::from_chars(line.data(), line.data() + line.size(), hash, 16);
        if (res.ptr != line.data() + 8) {
            GC_ERROR("Error parsing hash file: {}", file_path.filename().string());
            return;
        }
        std::string_view str(line.begin() + 9, line.end()); // skip over hash and space character
        // just create a Name object, if GC_LOOKUP_ASSET_IDS is defined, this will add to the LUT
        (void)Name(str);
    }
}

} // namespace gc

template <>
struct std::hash<gc::Name> {
    constexpr std::size_t operator()(const gc::Name& key) const noexcept { return static_cast<size_t>(key.getHash()); }
};
