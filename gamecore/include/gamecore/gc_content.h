#pragma once

#include <cstdint>

#include <vector>
#include <span>
#include <unordered_map>

#include <mio/mmap.hpp>

#include "gamecore/gc_name.h"

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used and finds all .gcpak files
// - Assets are only looked up by their asset ID, a given asset could be found in any .gcpak file
// - All .gcpak files are mapped into memory, returned assets just point to a part of the mapped file

namespace gc {

struct PackageAssetInfo; // forward-dec

class Content {

    // mmap_source objects are non-copyable and non-moveable so a vector cannot be used
    std::vector<mio::ummap_source> m_package_file_maps;

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
    std::span<const uint8_t> findAsset(Name name) const;
};

} // namespace gc
