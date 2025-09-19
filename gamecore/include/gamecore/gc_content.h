#pragma once

#include <cstdint>

#include <vector>
#include <unordered_map>
#include <memory>

#include <mio/mmap.hpp>

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used and finds all .gcpak files
// - Uses game file resource IDs to look up assets in .gcpak files
// - Decompresses game data files if they need to be (WIP)

namespace gc {

struct PackageAssetInfo; // forward-dec

class Content {

    // mmap_source objects are non-copyable and non-moveable so a vector cannot be used
    std::vector<std::unique_ptr<mio::mmap_source>> m_package_file_maps;

    std::unordered_map<uint32_t, PackageAssetInfo> m_asset_infos;

public:
    Content();
    Content(const Content&) = delete;
    Content(Content&&) = delete;

    ~Content();

    Content& operator=(const Content&) = delete;
    Content& operator=(Content&&) = delete;

    /* use gc::strToName(std::string_view str) */
    /* This function is thread-safe */
    /* Returns empty vector on failure */
    std::vector<uint8_t> loadAsset(std::uint32_t name) const;
};

} // namespace gc
