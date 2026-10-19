#include "terrain_compiler.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <bit>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <gclog/gclog.h>

#include <gamecore/gc_stopwatch.h>
#include <gamecore/gc_terrain_builder.h>
#include <gamecore/gc_units.h>

namespace {

struct HeightGrid {
    uint32_t width{};
    uint32_t height{};
    std::vector<float> heights{}; // rows first, from y = 0. NaN where there is no data
    float spacing{};              // zero if the file doesn't say
};

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

// The keywords of a PDS3 label. Those inside an OBJECT are prefixed with its name and a dot, e.g. "IMAGE.LINES"
std::optional<std::unordered_map<std::string, std::string>> readPdsLabel(std::ifstream& file)
{
    std::unordered_map<std::string, std::string> keywords{};
    std::vector<std::string> objects{};
    std::string line{};
    bool ended = false;
    while (std::getline(file, line)) {
        const std::string_view text = trim(line);
        if (text == "END") {
            ended = true;
            break;
        }
        const size_t equals = text.find('=');
        if (equals == std::string_view::npos || text.starts_with("/*")) {
            continue;
        }
        const std::string key(trim(text.substr(0, equals)));
        std::string value(trim(text.substr(equals + 1)));
        // a quoted value can go on over several lines
        if (!value.empty() && value.front() == '"' && std::count(value.begin(), value.end(), '"') % 2 == 1) {
            while (std::getline(file, line)) {
                value += " " + std::string(trim(line));
                if (line.find('"') != std::string::npos) {
                    break;
                }
            }
        }
        if (key == "OBJECT") {
            objects.push_back(value);
            continue;
        }
        if (key == "END_OBJECT") {
            if (!objects.empty()) {
                objects.pop_back();
            }
            continue;
        }
        std::string full_key{};
        for (const std::string& object : objects) {
            full_key += object + ".";
        }
        keywords[full_key + key] = value;
        if (file.tellg() > 1024 * 1024) {
            break; // that's no label
        }
    }
    if (!ended) {
        return {};
    }
    return keywords;
}

std::optional<HeightGrid> readPdsImage(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        GC_ERROR("Failed to open {}", name);
        return {};
    }
    const auto label = readPdsLabel(file);
    if (!label) {
        GC_ERROR("{}: isn't a PDS3 image (there is no label at the start of it)", name);
        return {};
    }
    const auto get = [&](const std::string& key) -> std::string {
        const auto it = label->find(key);
        return (it != label->end()) ? it->second : std::string{};
    };

    const long long record_bytes = std::atoll(get("RECORD_BYTES").c_str());
    const long long lines = std::atoll(get("IMAGE.LINES").c_str());
    const long long samples = std::atoll(get("IMAGE.LINE_SAMPLES").c_str());
    const std::string sample_type = get("IMAGE.SAMPLE_TYPE");
    const long long sample_bits = std::atoll(get("IMAGE.SAMPLE_BITS").c_str());
    const std::string image_pointer = get("^IMAGE");
    if (lines < 2 || samples < 2 || lines > 1000000 || samples > 1000000 || image_pointer.empty()) {
        GC_ERROR("{}: the label doesn't say how big the image is, or where it is", name);
        return {};
    }
    if (sample_type != "PC_REAL" || sample_bits != 32) {
        GC_ERROR("{}: has samples of type {} ({} bits). Only 32 bit PC_REAL (little endian floats) can be read", name, sample_type, sample_bits);
        return {};
    }
    if (std::atoll(get("IMAGE.BANDS").c_str()) > 1) {
        GC_ERROR("{}: has more than one band", name);
        return {};
    }
    // ^IMAGE is the record that the image starts at (counting from 1), or a byte (counting from 1) if it says <BYTES>
    long long offset = std::atoll(image_pointer.c_str()) - 1;
    if (image_pointer.find("<BYTES>") == std::string::npos) {
        offset *= record_bytes;
    }

    const float scaling = get("IMAGE.SCALING_FACTOR").empty() ? 1.0f : std::strtof(get("IMAGE.SCALING_FACTOR").c_str(), nullptr);
    const float base = std::strtof(get("IMAGE.OFFSET").c_str(), nullptr);
    // e.g. 16#FF7FFFFB#: the bits of the float that means "no data"
    std::optional<uint32_t> missing_bits{};
    if (const std::string missing = get("IMAGE.MISSING_CONSTANT"); missing.starts_with("16#")) {
        missing_bits = static_cast<uint32_t>(std::strtoul(missing.c_str() + 3, nullptr, 16));
    }

    HeightGrid grid{};
    grid.width = static_cast<uint32_t>(samples);
    grid.height = static_cast<uint32_t>(lines);
    grid.spacing = std::strtof(get("IMAGE_MAP_PROJECTION.MAP_SCALE").c_str(), nullptr);
    if (get("IMAGE_MAP_PROJECTION.MAP_SCALE").find("<KM") != std::string::npos) {
        grid.spacing *= 1000.0f;
    }
    grid.heights.resize(static_cast<size_t>(grid.width) * grid.height);

    file.clear();
    file.seekg(offset);
    std::vector<uint32_t> row(grid.width);
    size_t missing_count = 0;
    for (uint32_t line = 0; line < grid.height; ++line) {
        if (!file.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(uint32_t)))) {
            GC_ERROR("{}: ends before its last line", name);
            return {};
        }
        // The first line is the top of the image (north), which is +y
        float* const out = grid.heights.data() + static_cast<size_t>(grid.height - 1 - line) * grid.width;
        for (uint32_t i = 0; i < grid.width; ++i) {
            const float value = std::bit_cast<float>(row[i]);
            if ((missing_bits && row[i] == *missing_bits) || !std::isfinite(value) || value < -1.0e30f) {
                out[i] = std::numeric_limits<float>::quiet_NaN();
                ++missing_count;
            }
            else {
                out[i] = base + value * scaling;
            }
        }
    }
    GC_INFO("{}: {} x {} samples ({} of them have no data), {} m apart", name, grid.width, grid.height, missing_count, grid.spacing);
    return grid;
}

std::optional<HeightGrid> readPfm(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        GC_ERROR("Failed to open {}", name);
        return {};
    }
    std::string type{};
    long long width{}, height{};
    float scale{};
    file >> type >> width >> height >> scale;
    if (!file || type != "Pf" || width < 2 || height < 2 || width > 1000000 || height > 1000000) {
        GC_ERROR("{}: isn't a PFM file with one channel", name);
        return {};
    }
    file.get(); // the one whitespace character after the header

    HeightGrid grid{};
    grid.width = static_cast<uint32_t>(width);
    grid.height = static_cast<uint32_t>(height);
    grid.heights.resize(static_cast<size_t>(grid.width) * grid.height);
    if (!file.read(reinterpret_cast<char*>(grid.heights.data()), static_cast<std::streamsize>(grid.heights.size() * sizeof(float)))) {
        GC_ERROR("{}: is shorter than its header says", name);
        return {};
    }
    // (rows go from the bottom up, which is +y, as they are wanted)
    if (scale > 0.0f) {
        for (float& h : grid.heights) {
            const uint32_t bits = std::bit_cast<uint32_t>(h);
            h = std::bit_cast<float>((bits >> 24) | ((bits >> 8) & 0xff00u) | ((bits << 8) & 0xff0000u) | (bits << 24));
        }
    }
    for (float& h : grid.heights) {
        if (!std::isfinite(h) || h < -1.0e30f) {
            h = std::numeric_limits<float>::quiet_NaN();
        }
    }
    GC_INFO("{}: {} x {} samples", name, grid.width, grid.height);
    return grid;
}

} // namespace

bool compileTerrain(const std::filesystem::path& path, const TerrainCompileOptions& options, std::vector<Asset>& assets_out)
{
    const std::string name = path.filename().string();
    const gc::Stopwatch stopwatch{};

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::optional<HeightGrid> grid{};
    if (extension == ".img") {
        grid = readPdsImage(path);
    }
    else if (extension == ".pfm") {
        grid = readPfm(path);
    }
    else {
        GC_ERROR("{}: terrains can only be made from .IMG (PDS3) and .pfm files", name);
        return false;
    }
    if (!grid) {
        return false;
    }

    gc::TerrainBuildInput input{};
    input.width = grid->width;
    input.height = grid->height;
    input.heights = grid->heights;
    input.sample_spacing = (options.spacing > 0.0f) ? options.spacing : ((grid->spacing > 0.0f) ? grid->spacing : 1.0f);
    input.tile_quads = options.tile_quads;
    input.fill_level = options.fill_level;

    gc::TerrainBuildInfo info{};
    std::string error{};
    std::vector<uint8_t> data = gc::makeTerrainData(input, &info, &error);
    if (data.empty()) {
        GC_ERROR("{}: {}", name, error);
        return false;
    }

    const std::string asset_name = options.name.empty() ? name + ".terrain" : options.name;
    std::string per_level{};
    for (uint32_t level = 0; level < info.level_count; ++level) {
        per_level += std::format("{}{}", (level > 0) ? ", " : "", info.nodes_per_level[level]);
    }
    GC_INFO("{}: made {} ({}) in {}: {} levels, {} x {} top level tiles, {} tiles ({} from level 0 up). Heights {:.1f} to {:.1f} m, in steps of "
            "{:.2f} mm. {} samples were filled in",
            name, asset_name, gc::bytesToHumanReadable(data.size()), stopwatch, info.level_count, info.root_count_x, info.root_count_y, info.node_count,
            per_level, info.min_height, info.max_height, info.height_step * 1000.0f, info.filled_samples);
    assets_out.push_back(makeAsset(asset_name, std::move(data), gcpak::GcpakAssetType::TERRAIN));
    return true;
}
