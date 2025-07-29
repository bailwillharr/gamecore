#include "gamecore/gc_name.h"

#include <format>
#include <string>
#include <fstream>

#include "gamecore/gc_logger.h"

namespace gc {

#ifdef GC_LOOKUP_ASSET_IDS
static std::unordered_map<Name, std::string> s_id_lut{};

void loadAssetIDTable(const std::filesystem::path& file_path)
{
    std::ifstream file(file_path);
    if (!file) {
        GC_ERROR("Failed to open file: {}", file_path.filename().string());
        return;
    }
    std::string line{};
    file.seekg(0);
    while (std::getline(file, line)) {
        std::uint32_t hash;
        std::from_chars_result res = std::from_chars(line.data(), line.data() + line.size(), hash, 16);
        if (res.ptr != line.data() + 8) {
            GC_ERROR("Error parsing hash file: {}", file_path.filename().string());
            return;
        }
        std::string_view name(line.begin() + 9, line.end()); // skip over hash and space character
        s_id_lut.emplace(hash, name);
    }
}
#else
void loadAssetIDTable(const std::filesystem::path& file_path) { (void)file_path; }
#endif

std::string assetIDToStr(Name id)
{
#ifdef GC_LOOKUP_ASSET_IDS
    if (s_id_lut.contains(id)) {
        return s_id_lut.at(id);
    }
    else {
        return std::format("{:#08x}", id);
    }
#else
    return std::format("{:#08x}", id);
#endif
}

} // namespace gc
