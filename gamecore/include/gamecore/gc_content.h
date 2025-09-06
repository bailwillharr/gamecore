#pragma once

#include <cstdint>

#include <filesystem>
#include <string_view>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <tuple>
#include <mutex>

#include "gamecore/gc_gcpak.h"

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used and finds all .gcpak files
// - Uses game file resource IDs to look up assets in .gcpak files
// - Decompresses game data files if they need to be (WIP)

namespace gc {

struct PackageAssetInfo; // forward-dec

class Content {
    std::vector<std::ifstream> m_package_files;
    std::vector<std::mutex> m_package_file_mutexes;

    std::unordered_map<std::uint32_t, PackageAssetInfo> m_asset_infos;

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
    std::vector<uint8_t> loadAsset(std::uint32_t name);
};

} // namespace gc