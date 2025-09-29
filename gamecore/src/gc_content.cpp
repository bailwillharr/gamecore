#include "gamecore/gc_content.h"

#include <cstring>

#include <filesystem>
#include <optional>
#include <sstream>

#include <gcpak/gcpak.h>

#include <SDL3/SDL_filesystem.h>

#include <mio/mmap.hpp>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_assert.h"

namespace gc {

struct PackageAssetInfo {
    unsigned int file_index;
    gcpak::GcpakAssetEntry entry;
};

static std::optional<std::filesystem::path> findContentDir()
{
    const char* const base_path = SDL_GetBasePath();
    if (base_path) {
        const std::filesystem::path content_dir = std::filesystem::path(base_path) / "content";
        if (std::filesystem::is_directory(content_dir)) {
            return content_dir;
        }
        else {
            GC_ERROR("Failed to find content dir: {} is not a directory", content_dir.string());
            return {};
        }
    }
    else {
        GC_ERROR("Failed to find content dir: SDL_GetBasePath() error: {}", SDL_GetError());
        return {};
    }
}

// returns ummap_source and number of entries in file
static std::optional<std::pair<mio::ummap_source, std::uint32_t>> openAndValidateGcpak(const std::filesystem::path& file_path)
{
    std::error_code err;
    mio::ummap_source file{};
    file.map(file_path.string(), err);
    if (err) {
        GC_ERROR("Failed to map file: {}, code: {}", file_path.filename().string(), err.message());
        return {};
    }

    if (file.size() < sizeof(gcpak::GcpakHeader)) {
        GC_ERROR("Gcpak file too small: {}", file_path.filename().string());
        return {};
    }

    std::stringstream ss{};
    ss.write(reinterpret_cast<const char*>(file.data()), gcpak::GcpakHeader::getSerializedSize());
    GC_ASSERT(ss); // this shouldn't ever fail
    ss.seekg(0);
    auto header = gcpak::GcpakHeader::deserialize(ss);

    if (header.format_identifier != gcpak::GCPAK_VALID_IDENTIFIER) {
        GC_ERROR("Gcpak file header invalid: {}, got '{}'", file_path.filename().string(),
                 std::string_view(reinterpret_cast<const char*>(header.format_identifier.data()), header.format_identifier.size()));
        return {};
    }

    if (header.format_version != gcpak::GCPAK_CURRENT_VERSION) {
        GC_ERROR("Gcpak file version unsupported: {}", file_path.filename().string());
        return {};
    }

    return std::make_pair(std::move(file), header.num_entries);
}

/* no bounds checking done, ensure index < header.num_entries */
static gcpak::GcpakAssetEntry getAssetEntry(const mio::ummap_source& map, const uint32_t index)
{
    const std::ptrdiff_t entry_location_in_map = map.size() - ((static_cast<size_t>(index) + 1) * gcpak::GcpakAssetEntry::getSerializedSize());
    GC_ASSERT(entry_location_in_map > 0);

    std::stringstream ss{};
    ss.write(reinterpret_cast<const char*>(map.data() + entry_location_in_map), gcpak::GcpakAssetEntry::getSerializedSize());
    return gcpak::GcpakAssetEntry::deserialize(ss);
}

Content::Content()
{
    auto content_dir_opt = findContentDir();
    if (content_dir_opt) { // if findContentDir() hasn't failed
        // Iterate through the .gcpak files found in content/
        for (const auto& dir_entry : std::filesystem::directory_iterator(content_dir_opt.value())) {
            if (dir_entry.is_regular_file() && dir_entry.path().extension() == std::string{".gcpak"}) {
                GC_DEBUG("Loading .gcpak file: {}:", dir_entry.path().filename().string());

                auto opt = openAndValidateGcpak(dir_entry.path()); // cannot be const as opt.get() has a unique_ptr which is moved from
                if (opt) {

                    // first attempt to load hash LUT file (loadAssetIDTable() does nothing in release builds)
                    std::filesystem::path hash_file_path = dir_entry.path();
                    hash_file_path.replace_extension("txt");
                    loadNameLookupTable(hash_file_path);

                    // pair in optional might be OTT?
                    auto& [file, num_entries] = opt.value();
                    const unsigned int file_index = static_cast<unsigned int>(m_package_file_maps.size());
                    for (uint32_t i = 0; i < num_entries; ++i) {
                        const auto asset_entry = getAssetEntry(file, i);
                        PackageAssetInfo info{};
                        info.entry = asset_entry;
                        info.file_index = file_index;
                        m_asset_infos.emplace(info.entry.crc32_id, info);
                        GC_DEBUG("    {} ({})", Name(info.entry.crc32_id).getString(), bytesToHumanReadable(info.entry.size));
                    }
                    m_package_file_maps.emplace_back(std::move(file)); // keep file handle
                }
            }
        }
    }
    GC_TRACE("Initialised content manager");
}

Content::~Content() { GC_TRACE("Destroying content manager..."); }

std::span<const uint8_t> Content::findAsset(Name name) const
{
    const auto it = m_asset_infos.find(name);
    if (it == m_asset_infos.cend()) [[unlikely]] {
        GC_ERROR("Asset {} not found in any .gcpak file", name.getString());
        return {};
    }
    const PackageAssetInfo& asset_info = it->second;

    //GC_ASSERT(asset_info.file_index < m_package_file_maps.size());

    const uint8_t* const asset_data = m_package_file_maps[asset_info.file_index].data() + asset_info.entry.offset;
    return std::span<const uint8_t>(asset_data, asset_info.entry.size);
}

} // namespace gc
