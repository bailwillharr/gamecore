#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <string>
#include <optional>

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_scancode.h>

struct SDL_Window; // forward-dec

namespace gc {

struct WindowInitInfo {
    bool load_vulkan;
    bool resizable;
};

enum class ButtonState : uint8_t {
    UP = 0, // subsequent state on frames after button release
    DOWN, // subsequent state on frames after button press
    JUST_RELEASED, // button was just released
    JUST_PRESSED, // button was just pressed
};

class Window {
    SDL_Window* m_window_handle{};

    std::vector<SDL_DisplayMode> m_display_modes{};
    const SDL_DisplayMode* m_desktop_display_mode{};

    bool m_should_quit = false;
    bool m_is_fullscreen = false;

    std::array<ButtonState, SDL_SCANCODE_COUNT> m_keyboard_state{}; // zero initialisation sets all keys to ButtonState::UP

    std::atomic<bool> m_just_resized = false;

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

    void setIsResizable(bool resizable);

    // This method may fail but the window will remain usable.
    // If width or height == 0, fullscreen == true will use desktop resolution and fullscreen == false will maximise the window
    void setSize(uint32_t width, uint32_t height, bool fullscreen);
    
    std::array<int, 2> getSize() const;
    bool getIsFullscreen() const;

    // Keybard and mouse input
    bool getKeyDown(SDL_Scancode key) const;
    bool getKeyUp(SDL_Scancode key) const;
    bool getKeyPress(SDL_Scancode key) const;
    bool getKeyRelease(SDL_Scancode key) const;

    /* THREAD-SAFE FUNCTIONS */

    // This function must be thread-safe as it is used by the render thread to know when to recreate swapchain
    bool justResized() const;

private:
    std::optional<SDL_DisplayMode> findDisplayMode(int width, int height) const;
};

} // namespace gc