#pragma once

#include <cstdint>

#include <span>
#include <unordered_map>
#include <filesystem>

#include <gctemplates/gct_static_vector.h>

#include <gcpak/gcpak.h>

#include <mio/mmap.hpp>

#include "gamecore/gc_name.h"

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used and finds all .gcpak files
// - Assets are only looked up by their asset ID, a given asset could be found in any .gcpak file
// - All .gcpak files are mapped into memory, returned assets just point to a part of the mapped file

namespace gc {

struct AssetView {
    std::span<const uint8_t> data;
    gcpak::GcpakAssetType type;
};

class Content {

    struct PackageAssetInfo {
        unsigned int file_index;
        gcpak::GcpakAssetEntry entry;
    };

    static constexpr uint32_t MAX_PAK_FILES = 8;
    gct::static_vector<mio::ummap_source, MAX_PAK_FILES> m_package_file_maps;

    std::unordered_map<Name, PackageAssetInfo> m_asset_infos;

public:
    // asset_files: array of filenames to open as asset files (with .gcpak extension).
    //  If empty, open all .gcpak files in the directory.
    Content(const std::filesystem::path& content_dir, std::span<const std::string> asset_files = {});
    Content(const Content&) = delete;
    Content(Content&&) = delete;

    ~Content();

    Content& operator=(const Content&) = delete;
    Content& operator=(Content&&) = delete;

    /* This function is thread-safe */
    /* Returns a non-owning view of the asset */
    /* On failure, AssetView::data is empty */
    AssetView findAsset(Name name) const;

    /* This function is thread-safe */
    /* Whether the data is part of a mapped .gcpak file, as the data of every asset that findAsset() returns is. */
    /* Such data never changes or moves, and stays valid for as long as this object exists. */
    bool contains(std::span<const uint8_t> data) const;
};

} // namespace gc
