#pragma once

#include <cstdint>

#include <filesystem>
#include <string_view>
#include <vector>

// A wrapper around access to game engine assets:
// - Ensures the correct content directory is used
// - Maps game file resource identifiers to the corresponding files on disk
// - Decompresses game data files if they need to be

namespace gc {

class Content {
    std::filesystem::path m_content_dir;

public:
    Content();
    Content(const Content&) = delete;
    Content(Content&&) = delete;

    ~Content();

    Content& operator=(const Content&) = delete;
    Content& operator=(Content&&) = delete;

    /* use gc::assetID(std::string_view id) */
    std::vector<uint8_t> loadBin(std::uint32_t id);
};

} // namespace gc