#pragma once

#include <string>
#include <filesystem>

struct ImGuiContext; // forward-dec
struct SDL_Window;   // forward-dec
union SDL_Event;     // forward-dec

namespace gc {

struct RenderBackendInfo; // forward-dec
struct FrameState;        // forward-dec

class DebugUI {
    ImGuiContext* m_imgui_ctx{};
    std::string m_config_file{};

    // state variables

    bool m_show_demo{};
    bool m_clear_draw_data{};

public:
    bool active{};

public:
    DebugUI(SDL_Window* window, const RenderBackendInfo& render_backend_info, const std::filesystem::path& config_file);
    DebugUI(const DebugUI&) = delete;
    DebugUI(DebugUI&&) = delete;

    ~DebugUI();

    DebugUI& operator=(const DebugUI&) = delete;
    DebugUI& operator=(DebugUI&&) = delete;

    // Call every frame after Window::processEvents() and before RenderBackend::submitFrame()
    void update(FrameState& frame_state);

    static void windowEventInterceptor(SDL_Event& ev);
};

} // namespace gc
