#pragma once

#include <cstdint>

#include <vector>
#include <span>
#include <unordered_map>

#include <gctemplates/gct_static_vector.h>

#include <gcpak/gcpak.h>

#include <mio/mmap.hpp>

#include "gamecore/gc_name.h"

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used and finds all .gcpak files
// - Assets are only looked up by their asset ID, a given asset could be found in any .gcpak file
// - All .gcpak files are mapped into memory, returned assets just point to a part of the mapped file

namespace gc {

struct PackageAssetInfo; // forward-dec

class Content {

    static constexpr uint32_t MAX_PAK_FILES = 8;
    gct::static_vector<mio::ummap_source, MAX_PAK_FILES> m_package_file_maps;

    std::unordered_map<Name, PackageAssetInfo> m_asset_infos;

public:
    Content();
    Content(const Content&) = delete;
    Content(Content&&) = delete;

    ~Content();

    Content& operator=(const Content&) = delete;
    Content& operator=(Content&&) = delete;

    /* This function is thread-safe */
    /* Returns a non-owning view of the asset */
    /* Returns empty span on failure */
    /* The asset type is only checked in debug builds */
    std::span<const uint8_t> findAsset(Name name, gcpak::GcpakAssetType type) const;
};

} // namespace gc
