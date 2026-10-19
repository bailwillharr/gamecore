#include "hud.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <imgui.h>

#include <gamecore/gc_debug_ui.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_terrain_system.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "beacons.h"
#include "components.h"
#include "explorer.h"

static constexpr ImGuiWindowFlags OVERLAY_FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoInputs;

static const char* getCompassPoint(float bearing_degrees)
{
    static constexpr std::array<const char*, 8> POINTS{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int index = static_cast<int>(std::floor((bearing_degrees + 22.5f) / 45.0f));
    return POINTS[static_cast<size_t>(((index % 8) + 8) % 8)];
}

HudSystem::HudSystem(gc::World& world, gc::DebugUI& debug_ui, gc::TerrainSystem& terrain_system, const gc::ResourceTerrain& terrain,
                     const ExplorerSystem* explorer_system, const BeaconSystem& beacons, gc::Entity explorer)
    : gc::System(world),
      m_debug_ui(debug_ui),
      m_terrain_system(terrain_system),
      m_terrain(terrain),
      m_explorer_system(explorer_system),
      m_beacons(beacons),
      m_explorer(explorer)
{
}

void HudSystem::onUpdate(gc::FrameState& frame_state)
{
    if (!ImGui::GetCurrentContext() || !frame_state.window_state) {
        return;
    }
    const gc::WindowState& input = *frame_state.window_state;
    gc::TerrainSystem::Settings& settings = m_terrain_system.getSettings();
    if (input.getIsMouseCaptured()) {
        if (input.getKeyPress(SDL_SCANCODE_H)) {
            m_show_help = !m_show_help;
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

    const gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(m_explorer);
    const ExplorerComponent* const explorer = m_world.getComponent<ExplorerComponent>(m_explorer);
    if (!transform) {
        return;
    }
    const glm::vec3 eyes = transform->getPosition();
    const float ground = m_terrain.getHeight(eyes.x, eyes.y).value_or(eyes.z);

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const ImVec2 work_pos = viewport->WorkPos;
    const ImVec2 work_size = viewport->WorkSize;

    // Markers over the beacons, so that they can be found from a long way off
    const glm::mat4 view_projection = frame_state.draw_data.getProjectionMatrix() * frame_state.draw_data.getViewMatrix();
    const glm::vec2 window_size{input.getWindowSize()};
    ImDrawList* const draw_list = ImGui::GetBackgroundDrawList(); // (under the windows)
    const BeaconComponent* nearest = nullptr;
    float nearest_distance = 0.0f;
    glm::vec3 nearest_position{};
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
        const glm::vec2 screen = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * window_size;
        if (screen.x < 0.0f || screen.y < 0.0f || screen.x > window_size.x || screen.y > window_size.y) {
            return;
        }
        const ImU32 color = beacon.found ? IM_COL32(110, 255, 100, 200) : IM_COL32(90, 220, 255, 230);
        draw_list->AddCircle(ImVec2(screen.x, screen.y), 9.0f, color, 0, 2.0f);
        const std::string label = (distance < 1000.0f) ? std::format("{:.0f} m", distance) : std::format("{:.1f} km", distance / 1000.0f);
        draw_list->AddText(ImVec2(screen.x + 12.0f, screen.y - 8.0f), color, label.c_str());
    });

    // Where the explorer is, top left
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("Explorer", nullptr, OVERLAY_FLAGS)) {
        ImGui::TextUnformatted("MARS - a fissure east of Olympus Mons, 17.2 N 233.3 E");
        ImGui::TextDisabled("HiRISE DTM DTEEC_046252_1975_046107_1975_A01, 1 m apart, at 1:1");
        ImGui::Separator();
        const bool flying = explorer && explorer->flying;
        ImGui::Text("%s", !m_explorer_system ? "Flythrough" : (flying ? "Flying" : "Walking"));
        if (m_explorer_system) {
            ImGui::SameLine();
            ImGui::Text("  %.1f m/s (%.0f km/h)", m_explorer_system->getSpeed(), m_explorer_system->getSpeed() * 3.6f);
        }
        ImGui::Text("%.2f km east, %.2f km north", eyes.x / 1000.0f, eyes.y / 1000.0f);
        ImGui::Text("Elevation %.1f m (%.1f m above the ground)", eyes.z - EXPLORER_EYE_HEIGHT, eyes.z - EXPLORER_EYE_HEIGHT - ground);
        ImGui::Separator();
        ImGui::Text("Survey beacons found: %u of %u", m_beacons.getFoundCount(), m_beacons.getTotalCount());
        if (nearest) {
            const glm::vec2 to = glm::vec2(nearest_position - eyes);
            const float bearing = std::fmod(glm::degrees(std::atan2(to.x, to.y)) + 360.0f, 360.0f);
            const std::string distance =
                (nearest_distance < 1000.0f) ? std::format("{:.0f} m", nearest_distance) : std::format("{:.2f} km", nearest_distance / 1000.0f);
            ImGui::Text("Nearest: %s, %s %s (%.0f degrees)", BeaconSystem::getBeaconName(*nearest).c_str(), distance.c_str(), getCompassPoint(bearing),
                        bearing);
        }
        else if (m_beacons.getCompletedAfter() > 0.0) {
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.4f, 1.0f), "Survey complete, in %.0f minutes %.0f seconds", std::floor(m_beacons.getCompletedAfter() / 60.0),
                               std::fmod(m_beacons.getCompletedAfter(), 60.0));
        }
    }
    ImGui::End();

    // How the terrain is doing, top right
    const gc::TerrainSystem::Stats& stats = m_terrain_system.getStats();
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + work_size.x - 10.0f, work_pos.y + 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("Terrain stats", nullptr, OVERLAY_FLAGS)) {
        ImGui::Text("%.0f fps (%.2f ms)", 1.0 / std::max(frame_state.average_frame_time, 1.0e-6), frame_state.average_frame_time * 1000.0);
        ImGui::Text("Terrain: %u patches, %.2f M triangles", stats.patches, static_cast<double>(stats.triangles) / 1.0e6);
        ImGui::Text("Tiles in GPU memory: %u of %u (%u arriving)", stats.resident, stats.slots, stats.loading);
        ImGui::Text("Streamed: %llu tiles", static_cast<unsigned long long>(stats.total_uploads));
        ImGui::Text("Detail x%.2f%s%s", settings.detail_scale, settings.show_levels ? ", levels coloured" : "", settings.freeze ? ", FROZEN" : "");
    }
    ImGui::End();

    if (m_show_help && m_explorer_system) {
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + work_size.y - 10.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("Controls", nullptr, OVERLAY_FLAGS)) {
            ImGui::TextUnformatted("Mouse: look    W A S D: walk    Shift: run    Space: jump");
            ImGui::TextUnformatted("F: fly / walk    flying: Space/E up, Q down, Shift x10, Ctrl x0.1");
            ImGui::TextUnformatted("R: back to the start");
            ImGui::TextUnformatted("L: colour the levels of detail    K: freeze them    [ ]: less / more detail");
            ImGui::TextUnformatted("F10: debug windows (Windows > Terrain)    H: hide this");
        }
        ImGui::End();
    }
}
