#pragma once

#include <deque>
#include <filesystem>
#include <string>

struct ImGuiContext;                               // forward-dec
struct SDL_Window;                                 // forward-dec
union SDL_Event;                                   // forward-dec
typedef struct VkCommandBuffer_T* VkCommandBuffer; // forward-dec

namespace gc {

struct RenderBackendInfo; // forward-dec
struct FrameState;        // forward-dec
class Content;            // forward-dec

// Owns ImGui. Anything can draw ImGui windows during the frame (a game's HUD, the editor's UI), and they get the mouse and keyboard
// whenever the mouse isn't captured by the game.
// The engine's own debug windows are different: they are hidden until F10 is pressed, which shows a menu bar along the top of the
// screen. Each debug window is listed in its Windows menu and stays closed until it is picked there, so they don't cover the game
// all at once. See getWindowOpen().
class DebugUI {
    struct DebugWindow {
        std::string name;
        bool open;
    };

    ImGuiContext* m_imgui_ctx{};
    std::string m_config_file{};

    // A deque, as pointers to the 'open' flags are handed out
    std::deque<DebugWindow> m_windows{};

    // state variables

    bool m_show_demo{};
    bool m_clear_draw_data{};
    bool m_ambient_light{true};
    float m_ambient_light_scale{1.0f};
    bool m_shadows{true};
    float m_exposure_compensation{0.0f}; // in EV: +1 doubles the brightness

public:
    // Whether the debug windows and their menu bar are shown (toggled with F10)
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

    // Draws the menu bar
    void update(FrameState& frame_state);

    // For a window that is only for debugging. Call it every frame, whether or not the window ends up being drawn, so that the
    // window is listed in the menu bar's Windows menu.
    // Returns null if the window shouldn't be drawn: the debug UI is hidden, or the window is closed. Otherwise returns the
    // window's 'open' flag, to pass to ImGui::Begin() so that the window gets a close button:
    //     if (bool* const open = debug_ui.getWindowOpen("Network")) {
    //         if (ImGui::Begin("Network", open)) { ... }
    //         ImGui::End();
    //     }
    bool* getWindowOpen(const char* name);

    // Gives the event to ImGui, and hides it from the game if ImGui used it.
    // While the mouse is captured the game has all the input, as there is no cursor to click on anything with.
    void windowEventInterceptor(SDL_Event& ev, bool mouse_captured);

    static bool postRenderCallback(VkCommandBuffer cmd);
};

} // namespace gc
