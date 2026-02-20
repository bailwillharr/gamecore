#pragma once

#include <string>
#include <filesystem>

struct ImGuiContext; // forward-dec
struct SDL_Window;   // forward-dec
union SDL_Event;     // forward-dec
typedef struct VkCommandBuffer_T* VkCommandBuffer; // forward-dec

namespace gc {

struct RenderBackendInfo; // forward-dec
struct FrameState;        // forward-dec
class Content;            // forward-dec

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

    // Call every frame after Window::processEvents()
    void newFrame();

    // Call every frame before RenderBackend::submitFrame()
    void render();

    void update(const FrameState& frame_state, const Content& content);

    static void windowEventInterceptor(SDL_Event& ev);
    static bool postRenderCallback(VkCommandBuffer cmd);
};

} // namespace gc
