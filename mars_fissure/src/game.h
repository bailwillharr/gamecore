#pragma once

#include <cstdint>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace gc {
class App;              // forward-dec
struct ResourceTerrain; // forward-dec
} // namespace gc

struct Options {
    std::optional<int> render_sync_mode{};
    float flythrough_seconds{};          // if not zero: fly a set route for this long, logging how fast it was drawn, then quit
    float detail_distance{300.0f};       // see TerrainComponent::m_detail_distance
    uint32_t cache_slots{3072};          // see TerrainSystem
    bool start_flying{false};
    bool show_levels{false};             // draw the terrain's levels of detail in colours
    std::optional<glm::vec2> spawn{};    // where to start, in metres from the terrain's corner (the south west)
    std::string log_file{};
};

// Command line:
//   syncmode=N          see gc::RenderSyncMode
//   --flythrough [s]    fly a set route over the terrain for s seconds (60 if not given), logging the frame times and what the
//                       terrain streamed, then quit
//   --detail metres     how far the ground is drawn at full detail (300)
//   --cache tiles       how many of the terrain's tiles fit in GPU memory at once (3072)
//   --fly               start flying
//   --levels            colour the terrain's levels of detail
//   --spawn x y         start here (metres east and north of the terrain's south west corner)
//   --log file
Options parseCommandLine(std::span<const char* const> args);

// Places on the terrain, worked out from its heights when the game starts
struct Sites {
    glm::vec3 deepest{};
    glm::vec3 highest{};
    std::vector<glm::vec3> others{};
    glm::vec3 spawn{};      // where the explorer's feet start
    float spawn_yaw{};      // facing the deepest point
};

// Only looks at ground that came from the scan, not the smooth surface that fills in around it
Sites findSites(const gc::ResourceTerrain& terrain, std::optional<glm::vec2> spawn);

// Returns the process exit code
int buildAndStartGame(gc::App& app, const Options& options);
