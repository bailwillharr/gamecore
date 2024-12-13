#pragma once

#include <array>

#include <cstdlib>
#include <cstdint>

namespace gc {

/*
 * The gcpak file format contains many game assets each of which can either be compressed or decompressed.
 *
 * Version 1
 *
 * File format layout:
 *  -- HEADER
 *  -- ASSET DATA
 *  -- ASSET DATA
 *  -- ASSET DATA
 *  -- ...
 *  -- ASSET 1 INFO ENTRY (crc32 id, compressed yes/no, uncompressed size, compressed size, offset)
 *  -- ASSET 2 INFO ENTRY
 *  -- ASSET 3 INFO ENTRY
 *  -- ...
 *
 * Max size of an asset is 4 GiB.
 * Max number of assets is UINT32_MAX + 1
 * Max size of the gcpak file is very large (64-bit offsets)
 */

struct GcpakHeader {
    std::array<std::uint8_t, 6> format_identifier; // null-terminated "GCPAK"
    std::uint16_t format_version;                  // currently 1
    std::uint32_t num_entries;
};

struct GcpakAssetEntry {
    std::size_t offset;              // absolute positition of start of asset data in the file
    std::uint32_t crc32_id;
    std::uint32_t size_uncompressed; // set to zero for no compression
    std::uint32_t size;              // size of data in file (compressed size if compression enabled)
};

} // namespace gc