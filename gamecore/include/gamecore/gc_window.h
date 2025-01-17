#pragma once

#include <array>
#include <vector>
#include <string>
#include <optional>

#include <SDL3/SDL_video.h>

struct SDL_Window; // forward-dec

namespace gc {

struct WindowInitInfo {
    bool load_vulkan;
    bool resizable;
};

class Window {
    SDL_Window* m_window_handle;
    std::vector<SDL_DisplayMode> m_display_modes;
    bool m_should_quit;
    bool m_is_fullscreen = false;

public:
    explicit Window(const WindowInitInfo& info);
    Window(const Window&) = delete;
    Window(Window&&) = delete;

    ~Window();

    Window& operator=(const Window&) = delete;
    Window& operator=(Window&&) = delete;

    SDL_Window* getHandle();

    void processEvents();

    // can be internally set by Alt+F4, X button, etc
    void setQuitFlag();
    bool shouldQuit() const;

    void setWindowVisibility(bool visible);
    void setTitle(const std::string& title);
    // Returns false on failure
    bool setSize(int width, int height, bool fullscreen);
    std::array<int, 2> getSize() const;
    void setIsResizable(bool resizable);

private:
    std::optional<SDL_DisplayMode> findDisplayMode(int width, int height) const;
};

} // namespace gc