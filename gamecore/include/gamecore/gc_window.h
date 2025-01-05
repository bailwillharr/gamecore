#pragma once

#include <vector>
#include <string>
#include <optional>

#include <SDL3/SDL_video.h>

struct SDL_Window; // forward-dec

namespace gc {

class Window {
    SDL_Window* m_window_handle;
    std::vector<SDL_DisplayMode> m_display_modes;
    bool m_should_quit;

public:
    Window();
    Window(const Window&) = delete;
    Window(Window&&) = delete;

    ~Window();

    Window& operator=(const Window&) = delete;
    Window& operator=(Window&&) = delete;

    void processEvents();

    // can be internally set by Alt+F4, X button, etc
    void setQuitFlag();
    bool shouldQuit();

    void setWindowVisibility(bool visible);
    void setTitle(const std::string& title);
    // Returns false on failure
    bool setSize(int width, int height, bool fullscreen);

private:
    std::optional<SDL_DisplayMode> findDisplayMode(int width, int height);

};

} // namespace gc