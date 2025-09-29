#include "gamecore/gc_name.h"

#include <format>
#include <string>
#include <fstream>
#include <unordered_map>

#include "gamecore/gc_logger.h"

namespace gc {

#ifdef GC_LOOKUP_ASSET_IDS
static std::unordered_map<Name, std::string> s_id_lut{};

void addNameLookup(Name name, const std::string& str) { s_id_lut.emplace(name, str); }

void loadNameLookupTable(const std::filesystem::path& file_path)
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
        addNameLookup(hash, std::string(name));
    }
}

void debugLogNameLookups()
{
    GC_DEBUG("All known Names:");
    for (auto [name, str] : s_id_lut) {
        GC_DEBUG("  {} {}", name, str);
    }
}
#else
void addNameLookup(Name, const std::string&) {}
void loadNameLookupTable(const std::filesystem::path&) {}
void debugLogNameLookups() {}
#endif

std::string nameToStr(Name id)
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
