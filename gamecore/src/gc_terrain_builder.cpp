#include "gamecore/gc_terrain_builder.h"

#include <cmath>
#include <cstring>

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <limits>
#include <thread>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "gamecore/gc_terrain.h"

namespace gc {

namespace {

// Calls fn(begin, end) for parts of [0, count) on several threads at once
template <typename F>
void parallelFor(size_t count, F&& fn)
{
    const size_t thread_count = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 32);
    if (count < 64 || thread_count == 1) {
        fn(size_t{0}, count);
        return;
    }
    const size_t chunk = (count + thread_count - 1) / thread_count;
    std::vector<std::thread> threads{};
    for (size_t begin = 0; begin < count; begin += chunk) {
        threads.emplace_back([&fn, begin, end = std::min(begin + chunk, count)]() { fn(begin, end); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

struct Grid {
    uint32_t width{};
    uint32_t height{};
    std::vector<float> values{};

    Grid() = default;
    Grid(uint32_t w, uint32_t h, float value) : width(w), height(h), values(static_cast<size_t>(w) * h, value) {}

    float at(uint32_t x, uint32_t y) const { return values[static_cast<size_t>(y) * width + x]; }
    float& at(uint32_t x, uint32_t y) { return values[static_cast<size_t>(y) * width + x]; }
};

// Fills the samples that aren't finite with a smooth surface that meets the ones around them. "Pull-push": the grid is averaged
// down to a single sample, leaving out what is missing, and then each level fills its gaps from the one above it.
void fillHoles(Grid& grid)
{
    struct Level {
        uint32_t width{};
        uint32_t height{};
        std::vector<float> value{};
        std::vector<float> weight{}; // how much of the level's samples came from the data, up to 1
    };
    std::vector<Level> pyramid{};

    // pull: each level is the weighted average of 2x2 samples of the one below. Level 0 is the grid, whose weights are 0 or 1
    {
        uint32_t width = grid.width;
        uint32_t height = grid.height;
        while (width > 1 || height > 1) {
            Level level{};
            level.width = (width + 1) / 2;
            level.height = (height + 1) / 2;
            level.value.resize(static_cast<size_t>(level.width) * level.height);
            level.weight.resize(level.value.size());
            const Level* const below = pyramid.empty() ? nullptr : &pyramid.back();
            parallelFor(level.height, [&](size_t begin, size_t end) {
                for (size_t y = begin; y < end; ++y) {
                    for (uint32_t x = 0; x < level.width; ++x) {
                        float sum = 0.0f;
                        float weight = 0.0f;
                        for (uint32_t b = 0; b < 2; ++b) {
                            for (uint32_t a = 0; a < 2; ++a) {
                                const uint32_t sx = 2 * x + a;
                                const uint32_t sy = 2 * static_cast<uint32_t>(y) + b;
                                if (sx >= width || sy >= height) {
                                    continue;
                                }
                                if (below) {
                                    const size_t i = static_cast<size_t>(sy) * width + sx;
                                    sum += below->value[i] * below->weight[i];
                                    weight += below->weight[i];
                                }
                                else if (const float v = grid.at(sx, sy); std::isfinite(v)) {
                                    sum += v;
                                    weight += 1.0f;
                                }
                            }
                        }
                        const size_t i = y * level.width + x;
                        level.value[i] = (weight > 0.0f) ? sum / weight : 0.0f;
                        level.weight[i] = std::min(weight, 1.0f);
                    }
                }
            });
            width = level.width;
            height = level.height;
            pyramid.push_back(std::move(level));
        }
    }

    // A sample of a level is between four of the level above it
    const auto sample_above = [](const Level& above, uint32_t x, uint32_t y) {
        const float fx = std::clamp((static_cast<float>(x) - 0.5f) * 0.5f, 0.0f, static_cast<float>(above.width - 1));
        const float fy = std::clamp((static_cast<float>(y) - 0.5f) * 0.5f, 0.0f, static_cast<float>(above.height - 1));
        const uint32_t x0 = static_cast<uint32_t>(fx);
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t x1 = std::min(x0 + 1, above.width - 1);
        const uint32_t y1 = std::min(y0 + 1, above.height - 1);
        const float wx = fx - static_cast<float>(x0);
        const float wy = fy - static_cast<float>(y0);
        const auto v = [&](uint32_t sx, uint32_t sy) { return above.value[static_cast<size_t>(sy) * above.width + sx]; };
        return (v(x0, y0) * (1.0f - wx) + v(x1, y0) * wx) * (1.0f - wy) + (v(x0, y1) * (1.0f - wx) + v(x1, y1) * wx) * wy;
    };

    // push: from the top down, the part of each sample that wasn't data comes from the level above
    for (size_t level = pyramid.size() - 1; level-- > 0;) {
        Level& here = pyramid[level];
        const Level& above = pyramid[level + 1];
        parallelFor(here.height, [&](size_t begin, size_t end) {
            for (size_t y = begin; y < end; ++y) {
                for (uint32_t x = 0; x < here.width; ++x) {
                    const size_t i = y * here.width + x;
                    if (here.weight[i] < 1.0f) {
                        here.value[i] = here.weight[i] * here.value[i] + (1.0f - here.weight[i]) * sample_above(above, x, static_cast<uint32_t>(y));
                    }
                }
            }
        });
    }
    if (!pyramid.empty()) {
        const Level& above = pyramid[0];
        parallelFor(grid.height, [&](size_t begin, size_t end) {
            for (size_t y = begin; y < end; ++y) {
                for (uint32_t x = 0; x < grid.width; ++x) {
                    float& v = grid.at(x, static_cast<uint32_t>(y));
                    if (!std::isfinite(v)) {
                        v = sample_above(above, x, static_cast<uint32_t>(y));
                    }
                }
            }
        });
    }
}

// The next level of detail: every other sample, each smoothed with its neighbours first (a 1-2-1 filter), so that the samples
// of the coarser level are at the even samples of this one.
Grid downsample(const Grid& grid)
{
    Grid result((grid.width - 1) / 2 + 1, (grid.height - 1) / 2 + 1, 0.0f);
    constexpr std::array<float, 3> WEIGHTS{0.25f, 0.5f, 0.25f};
    parallelFor(result.height, [&](size_t begin, size_t end) {
        for (size_t y = begin; y < end; ++y) {
            for (uint32_t x = 0; x < result.width; ++x) {
                float sum = 0.0f;
                for (int b = -1; b <= 1; ++b) {
                    const int sy = std::clamp(2 * static_cast<int>(y) + b, 0, static_cast<int>(grid.height) - 1);
                    for (int a = -1; a <= 1; ++a) {
                        const int sx = std::clamp(2 * static_cast<int>(x) + a, 0, static_cast<int>(grid.width) - 1);
                        sum += WEIGHTS[a + 1] * WEIGHTS[b + 1] * grid.at(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
                    }
                }
                result.at(x, static_cast<uint32_t>(y)) = sum;
            }
        }
    });
    return result;
}

// Per node of a level
template <typename T>
struct NodeGrid {
    uint32_t width{};
    uint32_t height{};
    std::vector<T> values{};

    NodeGrid() = default;
    NodeGrid(uint32_t w, uint32_t h, T value) : width(w), height(h), values(static_cast<size_t>(w) * h, value) {}

    T& at(uint32_t x, uint32_t y) { return values[static_cast<size_t>(y) * width + x]; }
    const T& at(uint32_t x, uint32_t y) const { return values[static_cast<size_t>(y) * width + x]; }
};

} // namespace

std::vector<uint8_t> makeTerrainData(const TerrainBuildInput& input, TerrainBuildInfo* info_out, std::string* error_out)
{
    const auto fail = [&](std::string message) {
        if (error_out) {
            *error_out = std::move(message);
        }
        return std::vector<uint8_t>{};
    };

    const uint32_t quads = input.tile_quads;
    if (quads < 4 || quads > 1024 || (quads & (quads - 1)) != 0) {
        return fail(std::format("The number of quads along a tile ({}) has to be a power of two from 4 to 1024", quads));
    }
    if (input.width < 2 || input.height < 2 || input.heights.size() != static_cast<size_t>(input.width) * input.height) {
        return fail(std::format("A {}x{} grid of heights needs {} of them, not {}", input.width, input.height,
                                static_cast<size_t>(input.width) * input.height, input.heights.size()));
    }
    if (!(input.sample_spacing > 0.0f)) {
        return fail("The distance between samples has to be more than zero");
    }

    // Levels: enough that a tile of the top level is about as big as the terrain's shorter side
    const uint32_t shorter_side = std::min(input.width, input.height) - 1;
    uint32_t level_count = 1;
    while (level_count < TERRAIN_MAX_LEVELS && (static_cast<uint64_t>(quads) << level_count) <= shorter_side) {
        ++level_count;
    }
    const uint32_t top = level_count - 1;
    const uint32_t root_samples = quads << top; // along a top level tile's side, at level 0
    const uint32_t root_count_x = (input.width - 1 + root_samples - 1) / root_samples;
    const uint32_t root_count_y = (input.height - 1 + root_samples - 1) / root_samples;
    const uint32_t fill_level = std::min(input.fill_level, top);

    // Level 0, made big enough for a whole number of top level tiles. What is outside the data is missing, as are its holes
    Grid level0(root_count_x * root_samples + 1, root_count_y * root_samples + 1, std::numeric_limits<float>::quiet_NaN());
    uint32_t data_min_x = UINT32_MAX, data_max_x = 0, data_min_y = UINT32_MAX, data_max_y = 0;
    size_t data_count = 0;
    // which level 0 nodes have data. A sample on the edge between nodes is in both
    NodeGrid<uint8_t> has_data(root_count_x << top, root_count_y << top, 0);
    for (uint32_t y = 0; y < input.height; ++y) {
        const float* const row = input.heights.data() + static_cast<size_t>(y) * input.width;
        float* const out = &level0.at(0, y);
        for (uint32_t x = 0; x < input.width; ++x) {
            if (!std::isfinite(row[x])) {
                continue;
            }
            out[x] = row[x];
            ++data_count;
            data_min_x = std::min(data_min_x, x);
            data_max_x = std::max(data_max_x, x + 1);
            data_min_y = std::min(data_min_y, y);
            data_max_y = std::max(data_max_y, y + 1);
            const uint32_t node_x = std::min(x / quads, has_data.width - 1);
            const uint32_t node_y = std::min(y / quads, has_data.height - 1);
            has_data.at(node_x, node_y) = 1;
            if (x % quads == 0 && x > 0) {
                has_data.at(node_x - 1, node_y) = 1;
            }
            if (y % quads == 0 && y > 0) {
                has_data.at(node_x, node_y - 1) = 1;
                if (x % quads == 0 && x > 0) {
                    has_data.at(node_x - 1, node_y - 1) = 1;
                }
            }
        }
    }
    if (data_count == 0) {
        return fail("None of the heights are numbers");
    }
    const uint32_t filled_samples = static_cast<uint32_t>(std::min<size_t>(level0.values.size() - data_count, UINT32_MAX));
    fillHoles(level0);

    // The levels of detail
    std::vector<Grid> levels{};
    levels.reserve(level_count);
    levels.push_back(std::move(level0));
    for (uint32_t level = 1; level < level_count; ++level) {
        levels.push_back(downsample(levels.back()));
    }

    // Heights are stored in 16 bits, from the lowest to the highest. The levels are rounded to what is stored, so that everything
    // worked out from them here (normals, bounds, skirts) is what the renderer and the collision queries will see.
    float min_height = std::numeric_limits<float>::max();
    float max_height = std::numeric_limits<float>::lowest();
    for (const float v : levels[0].values) {
        min_height = std::min(min_height, v);
        max_height = std::max(max_height, v);
    }
    const float height_offset = min_height;
    const float height_scale = (max_height > min_height) ? (max_height - min_height) / 65535.0f : 1.0e-6f;
    const auto quantise = [&](float h) { return static_cast<uint32_t>(std::clamp(std::round((h - height_offset) / height_scale), 0.0f, 65535.0f)); };
    for (Grid& grid : levels) {
        parallelFor(grid.values.size(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                grid.values[i] = height_offset + static_cast<float>(quantise(grid.values[i])) * height_scale;
            }
        });
    }

    // Where each sample of a level goes as it turns into the level above (see TerrainSystem): the surface that the level above
    // draws there. The cells of the level above are split along their diagonals, from (0, 0) to (1, 1).
    const auto morph_target = [&](uint32_t level, uint32_t x, uint32_t y) {
        if (level == top) {
            return levels[level].at(x, y);
        }
        const Grid& above = levels[level + 1];
        const uint32_t ax = x / 2;
        const uint32_t ay = y / 2;
        const bool odd_x = (x & 1) != 0;
        const bool odd_y = (y & 1) != 0;
        if (!odd_x && !odd_y) {
            return above.at(ax, ay);
        }
        const float other = above.at(ax + (odd_x ? 1 : 0), ay + (odd_y ? 1 : 0));
        return (above.at(ax, ay) + other) * 0.5f;
    };

    // Which nodes are stored: those with data, and every node of the levels from fill_level up. A node with data has a parent with
    // data, so every stored node's parent is stored.
    std::vector<NodeGrid<uint8_t>> stored(level_count);
    std::vector<NodeGrid<uint8_t>> data_flags(level_count);
    data_flags[0] = std::move(has_data);
    for (uint32_t level = 1; level < level_count; ++level) {
        const NodeGrid<uint8_t>& below = data_flags[level - 1];
        NodeGrid<uint8_t> flags(below.width / 2, below.height / 2, 0);
        for (uint32_t y = 0; y < flags.height; ++y) {
            for (uint32_t x = 0; x < flags.width; ++x) {
                flags.at(x, y) = below.at(2 * x, 2 * y) | below.at(2 * x + 1, 2 * y) | below.at(2 * x, 2 * y + 1) | below.at(2 * x + 1, 2 * y + 1);
            }
        }
        data_flags[level] = std::move(flags);
    }
    for (uint32_t level = 0; level < level_count; ++level) {
        stored[level] = data_flags[level];
        if (level >= fill_level) {
            std::fill(stored[level].values.begin(), stored[level].values.end(), uint8_t{1});
        }
    }

    // Bounds and errors of each stored node, from the bottom up. A node's error is how far what it draws (its heights, and where
    // they go as it turns into its parent) is from the heights of level 0 at the same places, or its children's, if worse.
    struct NodeStats {
        float min_height;
        float max_height;
        float error;
    };
    std::vector<NodeGrid<NodeStats>> stats(level_count);
    for (uint32_t level = 0; level < level_count; ++level) {
        NodeGrid<NodeStats> level_stats(stored[level].width, stored[level].height, NodeStats{0.0f, 0.0f, 0.0f});
        const Grid& grid = levels[level];
        parallelFor(level_stats.values.size(), [&](size_t begin, size_t end) {
            for (size_t n = begin; n < end; ++n) {
                const uint32_t node_x = static_cast<uint32_t>(n % level_stats.width);
                const uint32_t node_y = static_cast<uint32_t>(n / level_stats.width);
                if (!stored[level].at(node_x, node_y)) {
                    continue;
                }
                NodeStats s{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f};
                for (uint32_t j = 0; j <= quads; ++j) {
                    for (uint32_t i = 0; i <= quads; ++i) {
                        const uint32_t x = node_x * quads + i;
                        const uint32_t y = node_y * quads + j;
                        const float h = grid.at(x, y);
                        const float m = morph_target(level, x, y);
                        const float truth = levels[0].at(x << level, y << level);
                        s.min_height = std::min({s.min_height, h, m});
                        s.max_height = std::max({s.max_height, h, m});
                        s.error = std::max({s.error, std::abs(h - truth), std::abs(m - truth)});
                    }
                }
                if (level > 0) {
                    for (uint32_t child = 0; child < 4; ++child) {
                        const uint32_t cx = node_x * 2 + (child & 1);
                        const uint32_t cy = node_y * 2 + (child >> 1);
                        if (stored[level - 1].at(cx, cy)) {
                            const NodeStats& c = stats[level - 1].at(cx, cy);
                            s.min_height = std::min(s.min_height, c.min_height);
                            s.max_height = std::max(s.max_height, c.max_height);
                            s.error = std::max(s.error, c.error);
                        }
                    }
                }
                level_stats.at(node_x, node_y) = s;
            }
        });
        stats[level] = std::move(level_stats);
    }

    // Skirts. Two neighbouring tiles, drawn at any levels, are each within their errors of the true surface, so the gap between
    // them is no more than the sum of their errors. A tile's skirt hangs down by twice the worst error of any node that touches
    // it, at its own level (its 3x3 neighbours) or above (its parent's skirt). Errors already include those of the levels below.
    std::vector<NodeGrid<float>> skirts(level_count);
    for (uint32_t level = level_count; level-- > 0;) {
        const NodeGrid<NodeStats>& level_stats = stats[level];
        NodeGrid<float> level_skirts(level_stats.width, level_stats.height, 0.0f);
        for (uint32_t y = 0; y < level_skirts.height; ++y) {
            for (uint32_t x = 0; x < level_skirts.width; ++x) {
                float worst = (level < top) ? skirts[level + 1].at(x / 2, y / 2) : 0.0f;
                for (int b = -1; b <= 1; ++b) {
                    for (int a = -1; a <= 1; ++a) {
                        const int nx = static_cast<int>(x) + a;
                        const int ny = static_cast<int>(y) + b;
                        if (nx >= 0 && ny >= 0 && nx < static_cast<int>(level_skirts.width) && ny < static_cast<int>(level_skirts.height)) {
                            worst = std::max(worst, level_stats.at(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny)).error);
                        }
                    }
                }
                level_skirts.at(x, y) = worst;
            }
        }
        skirts[level] = std::move(level_skirts);
    }

    // Number the nodes: the top level first, then each node's children after all of the nodes before them (breadth first)
    struct NodeRecord {
        uint32_t level;
        uint32_t x;
        uint32_t y;
    };
    std::vector<NodeRecord> records{};
    std::vector<NodeGrid<uint32_t>> indices(level_count);
    for (uint32_t level = 0; level < level_count; ++level) {
        indices[level] = NodeGrid<uint32_t>(stored[level].width, stored[level].height, TERRAIN_NO_NODE);
    }
    for (uint32_t y = 0; y < root_count_y; ++y) {
        for (uint32_t x = 0; x < root_count_x; ++x) {
            indices[top].at(x, y) = static_cast<uint32_t>(records.size());
            records.push_back(NodeRecord{top, x, y});
        }
    }
    for (size_t n = 0; n < records.size(); ++n) {
        const NodeRecord record = records[n];
        if (record.level == 0) {
            continue;
        }
        for (uint32_t child = 0; child < 4; ++child) {
            const uint32_t cx = record.x * 2 + (child & 1);
            const uint32_t cy = record.y * 2 + (child >> 1);
            if (stored[record.level - 1].at(cx, cy)) {
                indices[record.level - 1].at(cx, cy) = static_cast<uint32_t>(records.size());
                records.push_back(NodeRecord{record.level - 1, cx, cy});
            }
        }
    }

    const size_t side = static_cast<size_t>(quads) + 1;
    const size_t tile_bytes = side * side * sizeof(uint32_t);
    const size_t total_size = sizeof(TerrainHeader) + records.size() * sizeof(TerrainNode) + records.size() * tile_bytes;
    if (total_size > UINT32_MAX) {
        return fail(std::format("The terrain would be {} bytes, but an asset can't be more than 4 GiB. Use a coarser grid", total_size));
    }

    TerrainHeader header{};
    header.version = TERRAIN_VERSION;
    header.tile_quads = quads;
    header.level_count = level_count;
    header.root_count_x = root_count_x;
    header.root_count_y = root_count_y;
    header.node_count = static_cast<uint32_t>(records.size());
    header.data_min_x = data_min_x;
    header.data_max_x = data_max_x;
    header.data_min_y = data_min_y;
    header.data_max_y = data_max_y;
    header.sample_spacing = input.sample_spacing;
    header.height_offset = height_offset;
    header.height_scale = height_scale;
    header.min_height = min_height;
    header.max_height = max_height;

    std::vector<uint8_t> data(total_size);
    std::memcpy(data.data(), &header, sizeof(header));
    uint8_t* const nodes_out = data.data() + sizeof(TerrainHeader);
    uint8_t* const tiles_out = nodes_out + records.size() * sizeof(TerrainNode);

    parallelFor(records.size(), [&](size_t begin, size_t end) {
        for (size_t n = begin; n < end; ++n) {
            const NodeRecord& record = records[n];
            const NodeStats& s = stats[record.level].at(record.x, record.y);
            const float cell_size = input.sample_spacing * static_cast<float>(1u << record.level);

            TerrainNode node{};
            node.min_height = s.min_height;
            node.max_height = s.max_height;
            node.skirt_depth = 2.0f * skirts[record.level].at(record.x, record.y) + 0.25f * cell_size;
            for (uint32_t child = 0; child < 4; ++child) {
                node.children[child] = (record.level > 0) ? indices[record.level - 1].at(record.x * 2 + (child & 1), record.y * 2 + (child >> 1))
                                                          : TERRAIN_NO_NODE;
            }
            node.flags = data_flags[record.level].at(record.x, record.y) ? TERRAIN_NODE_HAS_DATA : 0;
            std::memcpy(nodes_out + n * sizeof(TerrainNode), &node, sizeof(node));

            // The tile: each sample's height, and the normal of the level's surface there
            const Grid& grid = levels[record.level];
            uint32_t* const tile = reinterpret_cast<uint32_t*>(tiles_out + n * tile_bytes);
            for (uint32_t j = 0; j <= quads; ++j) {
                for (uint32_t i = 0; i <= quads; ++i) {
                    const uint32_t x = record.x * quads + i;
                    const uint32_t y = record.y * quads + j;
                    const uint32_t x0 = (x > 0) ? x - 1 : x;
                    const uint32_t x1 = std::min(x + 1, grid.width - 1);
                    const uint32_t y0 = (y > 0) ? y - 1 : y;
                    const uint32_t y1 = std::min(y + 1, grid.height - 1);
                    const float dx = (grid.at(x1, y) - grid.at(x0, y)) / (static_cast<float>(x1 - x0) * cell_size);
                    const float dy = (grid.at(x, y1) - grid.at(x, y0)) / (static_cast<float>(y1 - y0) * cell_size);
                    const glm::vec3 normal = glm::normalize(glm::vec3{-dx, -dy, 1.0f});
                    const auto encode = [](float v) { return static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(std::round(v * 127.0f)))); };
                    tile[j * side + i] = quantise(grid.at(x, y)) | (encode(normal.x) << 16) | (encode(normal.y) << 24);
                }
            }
        }
    });

    if (info_out) {
        info_out->level_count = level_count;
        info_out->root_count_x = root_count_x;
        info_out->root_count_y = root_count_y;
        info_out->node_count = static_cast<uint32_t>(records.size());
        info_out->nodes_per_level.assign(level_count, 0);
        for (const NodeRecord& record : records) {
            ++info_out->nodes_per_level[record.level];
        }
        info_out->min_height = min_height;
        info_out->max_height = max_height;
        info_out->height_step = height_scale;
        info_out->filled_samples = filled_samples;
    }
    return data;
}

} // namespace gc
