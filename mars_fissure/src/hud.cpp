#include "hud.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_debug_ui.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_terrain_system.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_ui.h>
#include <gamecore/gc_ui_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "beacons.h"
#include "components.h"
#include "explorer.h"

using namespace gc::literals;

static constexpr const char* MODEL_NAME = "mars_hud";

static const char* getCompassPoint(float bearing_degrees)
{
    static constexpr std::array<const char*, 8> POINTS{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int index = static_cast<int>(std::floor((bearing_degrees + 22.5f) / 45.0f));
    return POINTS[static_cast<size_t>(((index % 8) + 8) % 8)];
}

static std::string formatDistance(float metres, int km_decimals)
{
    return (metres < 1000.0f) ? std::format("{:.0f} m", metres) : std::format("{:.{}f} km", metres / 1000.0f, km_decimals);
}

// Sets a bound variable, and tells the data model if it changed (so that only what changed is laid out again)
template <typename T>
static void setVariable(Rml::DataModelHandle& model, const char* name, T& variable, const T& value)
{
    if (!(variable == value)) {
        variable = value;
        model.DirtyVariable(name);
    }
}

HudSystem::HudSystem(gc::World& world, gc::UI& ui, gc::DebugUI& debug_ui, gc::TerrainSystem& terrain_system, const gc::ResourceTerrain& terrain,
                     const ExplorerSystem* explorer_system, const BeaconSystem& beacons, gc::Entity explorer)
    : gc::System(world),
      m_ui(ui),
      m_debug_ui(debug_ui),
      m_terrain_system(terrain_system),
      m_terrain(terrain),
      m_explorer_system(explorer_system),
      m_beacons(beacons),
      m_explorer(explorer)
{
    // The data model has to exist before the document is loaded (by the UISystem, which is registered after this)
    Rml::DataModelConstructor model = m_ui.getContext().CreateDataModel(MODEL_NAME);
    if (!model) {
        gc::abortGame("Failed to create the HUD's data model");
    }
    if (auto marker = model.RegisterStruct<Marker>()) {
        marker.RegisterMember("x", &Marker::x);
        marker.RegisterMember("y", &Marker::y);
        marker.RegisterMember("label", &Marker::label);
        marker.RegisterMember("found", &Marker::found);
    }
    model.RegisterArray<std::vector<Marker>>();
    model.Bind("has_explorer", &m_has_explorer);
    model.Bind("show_help", &m_show_help);
    model.Bind("mode", &m_mode);
    model.Bind("speed", &m_speed);
    model.Bind("position", &m_position);
    model.Bind("elevation", &m_elevation);
    model.Bind("found", &m_found);
    model.Bind("total", &m_total);
    model.Bind("found_fraction", &m_found_fraction);
    model.Bind("has_nearest", &m_has_nearest);
    model.Bind("nearest", &m_nearest);
    model.Bind("complete", &m_complete);
    model.Bind("complete_text", &m_complete_text);
    model.Bind("fps", &m_fps);
    model.Bind("patches", &m_patches);
    model.Bind("tiles", &m_tiles);
    model.Bind("streamed", &m_streamed);
    model.Bind("detail", &m_detail);
    model.Bind("detail_warning", &m_detail_warning);
    model.Bind("markers", &m_markers);
    m_model = model.GetModelHandle();

    m_hud = m_world.createEntity("hud"_name);
    m_world.addComponent<gc::UIComponent>(m_hud).document = "mars_hud.rml"_name;
}

HudSystem::~HudSystem()
{
    // (the document stays open until the UISystem goes, which is after this. It is detached from the model)
    m_ui.getContext().RemoveDataModel(MODEL_NAME);
}

void HudSystem::onUpdate(gc::FrameState& frame_state)
{
    if (!frame_state.window_state) {
        return;
    }
    const gc::WindowState& input = *frame_state.window_state;
    gc::TerrainSystem::Settings& settings = m_terrain_system.getSettings();
    if (input.getIsMouseCaptured()) {
        if (input.getKeyPress(SDL_SCANCODE_H)) {
            setVariable(m_model, "show_help", m_show_help, !m_show_help);
        }
        if (input.getKeyPress(SDL_SCANCODE_L)) {
            settings.show_levels = !settings.show_levels;
        }
        if (input.getKeyPress(SDL_SCANCODE_K)) {
            settings.freeze = !settings.freeze;
        }
        if (input.getKeyPress(SDL_SCANCODE_LEFTBRACKET)) {
            settings.detail_scale = std::max(settings.detail_scale / 1.25f, 0.25f);
        }
        if (input.getKeyPress(SDL_SCANCODE_RIGHTBRACKET)) {
            settings.detail_scale = std::min(settings.detail_scale * 1.25f, 4.0f);
        }
    }

    if (bool* const open = m_debug_ui.getWindowOpen("Terrain")) {
        m_terrain_system.renderDebugWindow(open);
    }

    // How the terrain is doing
    const gc::TerrainSystem::Stats& stats = m_terrain_system.getStats();
    setVariable(m_model, "fps", m_fps,
                std::format("{:.0f} fps ({:.2f} ms)", 1.0 / std::max(frame_state.average_frame_time, 1.0e-6), frame_state.average_frame_time * 1000.0));
    setVariable(m_model, "patches", m_patches,
                std::format("Terrain: {} patches, {:.2f} M triangles", stats.patches, static_cast<double>(stats.triangles) / 1.0e6));
    setVariable(m_model, "tiles", m_tiles, std::format("Tiles in GPU memory: {} of {} ({} arriving)", stats.resident, stats.slots, stats.loading));
    setVariable(m_model, "streamed", m_streamed, std::format("Streamed: {} tiles", stats.total_uploads));
    setVariable(m_model, "detail", m_detail,
                std::format("Detail x{:.2f}{}{}", settings.detail_scale, settings.show_levels ? ", levels coloured" : "", settings.freeze ? ", FROZEN" : ""));
    setVariable(m_model, "detail_warning", m_detail_warning, settings.freeze);

    const gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(m_explorer);
    const ExplorerComponent* const explorer = m_world.getComponent<ExplorerComponent>(m_explorer);
    if (!transform) {
        return;
    }
    const glm::vec3 eyes = transform->getPosition();
    const float ground = m_terrain.getHeight(eyes.x, eyes.y).value_or(eyes.z);

    // Markers over the beacons, so that they can be found from a long way off
    const glm::mat4 view_projection = frame_state.draw_data.getProjectionMatrix() * frame_state.draw_data.getViewMatrix();
    const glm::vec2 screen_size{m_ui.getSize()}; // (the UI is laid out in pixels)
    const BeaconComponent* nearest = nullptr;
    float nearest_distance = 0.0f;
    glm::vec3 nearest_position{};
    std::vector<Marker> markers{};
    m_world.forEach<gc::TransformComponent, BeaconComponent>([&](gc::Entity, const gc::TransformComponent& t, const BeaconComponent& beacon) {
        const glm::vec3 lamp = t.getPosition() + glm::vec3{0.0f, 0.0f, 3.8f};
        const float distance = glm::distance(lamp, eyes);
        if (!beacon.found && (!nearest || distance < nearest_distance)) {
            nearest = &beacon;
            nearest_distance = distance;
            nearest_position = lamp;
        }
        const glm::vec4 clip = view_projection * glm::vec4(lamp, 1.0f);
        if (clip.w <= 0.0f) {
            return; // behind the camera
        }
        const glm::vec2 screen = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * screen_size;
        if (screen.x < 0.0f || screen.y < 0.0f || screen.x > screen_size.x || screen.y > screen_size.y) {
            return;
        }
        // (whole pixels, so that the labels' text stays sharp)
        markers.push_back(Marker{std::round(screen.x), std::round(screen.y), formatDistance(distance, 1), beacon.found});
    });
    setVariable(m_model, "markers", m_markers, markers);

    // Where the explorer is
    setVariable(m_model, "has_explorer", m_has_explorer, m_explorer_system != nullptr);
    const bool flying = explorer && explorer->flying;
    setVariable(m_model, "mode", m_mode, std::string(!m_explorer_system ? "Flythrough" : (flying ? "Flying" : "Walking")));
    if (m_explorer_system) {
        constexpr float MPH_PER_MPS = static_cast<float>(3600.0 * 1000.0 / 25.4 / 12.0 / 5280.0);
        setVariable(m_model, "speed", m_speed, std::format("{:.1f} m/s ({:.0f} mph)", m_explorer_system->getSpeed(), m_explorer_system->getSpeed() * MPH_PER_MPS));
    }
    setVariable(m_model, "position", m_position, std::format("{:.2f} km east, {:.2f} km north", eyes.x / 1000.0f, eyes.y / 1000.0f));
    setVariable(m_model, "elevation", m_elevation,
                std::format("{:.1f} m ({:.1f} m above the ground)", eyes.z - EXPLORER_EYE_HEIGHT, eyes.z - EXPLORER_EYE_HEIGHT - ground));

    // The survey
    const int found = static_cast<int>(m_beacons.getFoundCount());
    const int total = static_cast<int>(m_beacons.getTotalCount());
    setVariable(m_model, "found", m_found, found);
    setVariable(m_model, "total", m_total, total);
    setVariable(m_model, "found_fraction", m_found_fraction, (total > 0) ? static_cast<float>(found) / static_cast<float>(total) : 0.0f);
    setVariable(m_model, "has_nearest", m_has_nearest, nearest != nullptr);
    if (nearest) {
        const glm::vec2 to = glm::vec2(nearest_position - eyes);
        const float bearing = std::fmod(glm::degrees(std::atan2(to.x, to.y)) + 360.0f, 360.0f);
        setVariable(m_model, "nearest", m_nearest,
                    std::format("{}, {} {} ({:.0f} degrees)", BeaconSystem::getBeaconName(*nearest), formatDistance(nearest_distance, 2),
                                getCompassPoint(bearing), bearing));
    }
    const double completed_after = m_beacons.getCompletedAfter();
    setVariable(m_model, "complete", m_complete, !nearest && completed_after > 0.0);
    if (completed_after > 0.0) {
        setVariable(m_model, "complete_text", m_complete_text,
                    std::format("Survey complete, in {:.0f} minutes {:.0f} seconds", std::floor(completed_after / 60.0), std::fmod(completed_after, 60.0)));
    }
}
