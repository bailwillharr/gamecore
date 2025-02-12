#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <string>
#include <optional>

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_mouse.h>

struct SDL_Window; // forward-dec

namespace gc {

struct WindowInitInfo {
    bool load_vulkan;
    bool resizable;
};

enum class ButtonState : uint8_t {
    UP = 0,        // subsequent state on frames after button release
    DOWN,          // subsequent state on frames after button press
    JUST_RELEASED, // button was just released
    JUST_PRESSED,  // button was just pressed
};

enum class MouseButton : uint8_t {
    LEFT = SDL_BUTTON_LEFT - 1,
    MIDDLE = SDL_BUTTON_MIDDLE - 1,
    RIGHT = SDL_BUTTON_RIGHT - 1,
    X1 = SDL_BUTTON_X1 - 1,
    X2 = SDL_BUTTON_X2 - 1,
    COUNT = SDL_BUTTON_X2,
};

class Window {
    SDL_Window* m_window_handle{};

    std::array<ButtonState, SDL_SCANCODE_COUNT> m_keyboard_state{}; // zero initialisation sets all keys to ButtonState::UP
    std::array<ButtonState, static_cast<size_t>(MouseButton::COUNT)> m_mouse_button_state{};
    std::array<float, 2> m_mouse_position{};

    std::array<uint32_t, 2> m_window_size{};

    bool m_should_quit = false;
    bool m_is_fullscreen = false;
    bool m_resized_flag = false;

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

    std::array<uint32_t, 2> getSize() const;
    bool getIsFullscreen() const;

    // Keybard and mouse input
    bool getKeyDown(SDL_Scancode key) const;
    bool getKeyUp(SDL_Scancode key) const;
    bool getKeyPress(SDL_Scancode key) const;
    bool getKeyRelease(SDL_Scancode key) const;
    bool getButtonDown(MouseButton button) const;
    bool getButtonUp(MouseButton button) const;
    bool getButtonPress(MouseButton button) const;
    bool getButtonRelease(MouseButton button) const;

    // In window coordinates with origin at top-left
    std::array<float, 2> getMousePosition() const;
    // returns values from -1.0 to 1.0, left-to-right, bottom-to-top (GL style)
    std::array<double, 2> getMousePositionNorm() const;

    bool getResizedFlag() const;

};

} // namespace gc
