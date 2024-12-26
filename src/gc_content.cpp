#include "gamecore/gc_content.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>

#include "gamecore/gc_abort.h"
#include "gamecore/gc_disk_io.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_gcpak.h"
#include "gamecore/gc_asset_id.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_assert.h"

namespace gc {

struct PackageAssetInfo {
    unsigned int file_index;
    GcpakAssetEntry entry;
};

static std::optional<std::pair<std::ifstream, std::uint32_t>> openAndValidateGcpak(const std::filesystem::path& file_path)
{
    std::ifstream file(file_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        GC_ERROR("Failed to open file: {}", file_path.filename().string());
        return {};
    }

    GcpakHeader header{};
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(&header), sizeof(GcpakHeader));
    if (file.gcount() != sizeof(GcpakHeader)) {
        GC_ERROR("Failed to read gcpak header for file: {}, {}/{} bytes read", file_path.filename().string(), file.gcount(), sizeof(GcpakHeader));
        return {};
    }

    if (header.format_identifier != GCPAK_FORMAT_IDENTIFIER) {
        GC_ERROR("Gcpak file header invalid: {}, got '{}'", file_path.filename().string(),
                 std::string_view(reinterpret_cast<const char*>(header.format_identifier.data()), header.format_identifier.size()));
        return {};
    }

    if (header.format_version != GCPAK_FORMAT_VERSION) {
        GC_ERROR("Gcpak file version unsupported: {}", file_path.filename().string());
        return {};
    }

    return std::make_pair(std::move(file), header.num_entries);
}

/* no bounds checking done, ensure index < header.num_entries */
static std::optional<GcpakAssetEntry> getAssetEntry(std::ifstream& file, const int index)
{
    GcpakAssetEntry entry{};

    const std::streamoff offset = (-1LL - static_cast<std::streamoff>(index)) * sizeof(GcpakAssetEntry);
    file.seekg(offset, std::ios::end);

    file.read(reinterpret_cast<char*>(&entry), sizeof(GcpakAssetEntry));
    if (file.gcount() != sizeof(GcpakAssetEntry)) {
        GC_ERROR("file.read() failed to read gcpak asset entry! {}/{} bytes read", file.gcount(), sizeof(GcpakAssetEntry));
        return {};
    }
    else {
        return entry;
    }
}

Content::Content()
{
    auto content_dir_opt = findContentDir();
    if (content_dir_opt) { // if findContentDir() hasn't failed
        // Iterate through the .gcpak files found in content/
        for (const auto& dir_entry : std::filesystem::directory_iterator(content_dir_opt.value())) {
            if (dir_entry.is_regular_file() && dir_entry.path().extension() == std::string{".gcpak"}) {
                GC_DEBUG("Loading .gcpak file: {}:", dir_entry.path().filename().string());

                auto opt = openAndValidateGcpak(dir_entry.path());
                if (opt) {

                    // first attempt to load hash LUT file (loadAssetIDTable() does nothing in release builds)
                    std::filesystem::path hash_file_path = dir_entry.path();
                    hash_file_path.replace_extension("txt");
                    loadAssetIDTable(hash_file_path);

                    // pair in optional might be OTT?
                    auto& [file, num_entries] = opt.value();
                    const unsigned int file_index = static_cast<unsigned int>(m_package_files.size());
                    for (uint32_t i = 0; i < num_entries; ++i) {
                        const auto asset_entry = getAssetEntry(file, i);
                        if (asset_entry) {
                            PackageAssetInfo info{};
                            info.entry = asset_entry.value();
                            info.file_index = file_index;
                            m_asset_infos.emplace(info.entry.crc32_id, info); // crc32 is stored in both key and value here
                            GC_DEBUG("    {} ({})", nameFromID(info.entry.crc32_id), bytesToHumanReadable(info.entry.size));
                        }
                        else {
                            GC_ERROR("Failed to locate entry in {}, Skipping the rest of this file.", dir_entry.path().filename().string());
                            break;
                        }
                    }
                    m_package_files.emplace_back(std::move(file)); // keep file handle
                }
            }
        }
        m_package_file_mutexes = std::vector<std::mutex>(m_package_files.size());
    }
    GC_TRACE("Initialised content manager");
}

Content::~Content() { GC_TRACE("Shutting down content manager"); }

std::vector<uint8_t> Content::loadAsset(std::uint32_t id)
{

    // get asset info
    if (!m_asset_infos.contains(id)) {
        GC_ERROR("Asset {} not found in any .gcpak file", nameFromID(id));
        return {};
    }
    PackageAssetInfo asset_info = m_asset_infos[id];
    if (asset_info.entry.size_uncompressed != 0) {
        GC_ERROR("Asset {} is compressed which is not supported yet", nameFromID(id));
        return {};
    }

    // read file
    {
        GC_ASSERT(asset_info.file_index < m_package_file_mutexes.size());
        GC_ASSERT(asset_info.file_index < m_package_files.size());

        std::lock_guard lock(m_package_file_mutexes[asset_info.file_index]);
        auto& file = m_package_files[asset_info.file_index];
        file.seekg(asset_info.entry.offset, std::ios::beg);
        std::vector<std::uint8_t> data(asset_info.entry.size);
        file.read(reinterpret_cast<char*>(data.data()), asset_info.entry.size);
        if (file.gcount() != asset_info.entry.size) {
            GC_ERROR("file.read() failed to read asset {} from file! {}/{} bytes read", nameFromID(id), file.gcount(), asset_info.entry.size);
            return {};
        }
        else {
            return data;
        }
    }
}

} // namespace gc