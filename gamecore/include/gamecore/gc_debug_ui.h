#pragma once

#include <string>
#include <filesystem>

struct ImGuiContext; // forward-dec
struct SDL_Window;   // forward-dec

namespace gc {

class RenderBackend; // forward-dec

class DebugUI {
    ImGuiContext* m_imgui_ctx{};
    std::string m_config_file{};

    // state variables

    bool m_show_demo{};

public:
    bool active{};

public:
    DebugUI(SDL_Window* window, RenderBackend& render_backend, const std::filesystem::path& config_file);
    DebugUI(const DebugUI&) = delete;
    DebugUI(DebugUI&&) = delete;

    ~DebugUI();

    DebugUI& operator=(const DebugUI&) = delete;
    DebugUI& operator=(DebugUI&&) = delete;

    // Call every frame after Window::processEvents() and before RenderBackend::renderFrame()
    void update();
};

} // namespace gc