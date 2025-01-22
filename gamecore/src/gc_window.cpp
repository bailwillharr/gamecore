#include "gamecore/gc_window.h"

#include <span>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"

namespace gc {

static constexpr const char* INITIAL_TITLE = "Gamecore Game Window";
static constexpr int INITIAL_WIDTH = 1024;
static constexpr int INITIAL_HEIGHT = 768;

static void resetKeyboardState(std::span<ButtonState, SDL_SCANCODE_COUNT> keyboard_state)
{
    for (ButtonState& state : keyboard_state) {
        if (state == ButtonState::JUST_RELEASED) {
            state = ButtonState::UP;
        }
        else if (state == ButtonState::JUST_PRESSED) {
            state = ButtonState::DOWN;
        }
    }
}

static void resetMouseButtonState(std::span<ButtonState, static_cast<size_t>(MouseButton::COUNT)> mouse_button_state)
{
    for (ButtonState& state : mouse_button_state) {
        if (state == ButtonState::JUST_RELEASED) {
            state = ButtonState::UP;
        }
        else if (state == ButtonState::JUST_PRESSED) {
            state = ButtonState::DOWN;
        }
    }
}

Window::Window(const WindowInitInfo& info)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        GC_ERROR("SDL_InitSubSystem() error: {}", SDL_GetError());
        abortGame("Failed to initialise SDL video subsystem.");
    }
    SDL_WindowFlags window_flags{};
    window_flags |= SDL_WINDOW_HIDDEN; // window is shown later
    // no resize:
    window_flags |= info.resizable ? SDL_WINDOW_RESIZABLE : 0;
    window_flags |= info.load_vulkan ? SDL_WINDOW_VULKAN : 0;
    m_window_handle = SDL_CreateWindow(INITIAL_TITLE, INITIAL_WIDTH, INITIAL_HEIGHT, window_flags);
    if (!m_window_handle) {
        GC_ERROR("SDL_CreateWindow() error: {}", SDL_GetError());
        abortGame("Failed to create window.");
    }

    // Get fullscreen display modes for use later
    m_display_modes.clear();
    const SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
    if (display_id != 0) {

        int count{};
        SDL_DisplayMode** sdl_modes = SDL_GetFullscreenDisplayModes(display_id, &count);
        if (sdl_modes) {
            GC_ASSERT(count >= 0);
            m_display_modes.resize(count);
            for (int i = 0; i < count; ++i) {
                m_display_modes[i] = *(sdl_modes[i]);
            }
            SDL_free(sdl_modes);
        }
        else {
            GC_ERROR("SDL_GetFullscreenDisplayModes() failed: {}", SDL_GetError());
        }

        m_desktop_display_mode = SDL_GetDesktopDisplayMode(display_id);
        if (!m_desktop_display_mode) {
            GC_ERROR("SDL_GetDesktopDisplayMode() error: {}", SDL_GetError());
            // SDL_SetWindowFullscreenMode() supports NULL display mode so m_desktop_display_mode can still be used
        }
    }
    else {
        GC_ERROR("SDL_GetPrimaryDisplay() failed: {}", SDL_GetError());
        // it is safe to continue, there will just be no available fullscreen display modes
    }
}

Window::~Window()
{
    SDL_DestroyWindow(m_window_handle);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_Window* Window::getHandle() { return m_window_handle; }

void Window::setWindowVisibility(bool visible)
{
    if (visible) {
        if (!SDL_ShowWindow(m_window_handle)) {
            GC_ERROR("SDL_ShowWindow() error: {}", SDL_GetError());
        }
    }
    else {
        if (!SDL_HideWindow(m_window_handle)) {
            GC_ERROR("SDL_HideWindow() error: {}", SDL_GetError());
        }
    }
}

void Window::processEvents()
{
    ZoneScoped;

    resetKeyboardState(m_keyboard_state);
    resetMouseButtonState(m_mouse_button_state);
    m_resized_flag = false;

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                setQuitFlag();
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_resized_flag = true;
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                m_is_fullscreen = true;
                break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                m_is_fullscreen = false;
                break;
            case SDL_EVENT_KEY_DOWN: {
                ButtonState& state = m_keyboard_state[ev.key.scancode];
                if (state == ButtonState::UP) {
                    state = ButtonState::JUST_PRESSED;
                }
            } break;
            case SDL_EVENT_KEY_UP: {
                ButtonState& state = m_keyboard_state[ev.key.scancode];
                if (state == ButtonState::DOWN) {
                    state = ButtonState::JUST_RELEASED;
                }
            } break;
            case SDL_EVENT_MOUSE_MOTION:
                // handle mouse motion
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                ButtonState& state = m_mouse_button_state[ev.button.button - 1];
                if (state == ButtonState::UP) {
                    state = ButtonState::JUST_PRESSED;
                }
            } break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                ButtonState& state = m_mouse_button_state[ev.button.button - 1];
                if (state == ButtonState::DOWN) {
                    state = ButtonState::JUST_RELEASED;
                }
            } break;
                // handle mouse buttons
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // handle mouse wheel
                break;
                // handle gamepad / joystick here
                // handle clipboard here
                // handle drag and drop here
                // handle audio device here
        }
    }
}

void Window::setQuitFlag() { m_should_quit = true; }

bool Window::shouldQuit() const { return m_should_quit; }

void Window::setTitle(const std::string& title)
{
    if (!SDL_SetWindowTitle(m_window_handle, title.c_str())) {
        GC_ERROR("SDL_SetWindowTitle() failed: {}", SDL_GetError());
    }
}

void Window::setSize(uint32_t width, uint32_t height, bool fullscreen)
{
    if (!SDL_SetWindowFullscreen(m_window_handle, fullscreen)) {
        GC_ERROR("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
    }

    if (fullscreen) {
        const SDL_DisplayMode* mode_ptr{}; // can be null for windowed fullscreen
        if (width != 0 && height != 0) {
            if (auto mode = findDisplayMode(width, height); mode.has_value()) {
                mode_ptr = &mode.value();
            }
        }
        else {
            mode_ptr = m_desktop_display_mode;
        }
        if (!SDL_SetWindowFullscreenMode(m_window_handle, mode_ptr)) {
            GC_ERROR("SDL_SetWindowFullscreenMode() error: {}", SDL_GetError());
        }
    }
    else { // regular windowed mode

        if (width == 0 || height == 0) {
            if (!SDL_MaximizeWindow(m_window_handle)) {
                GC_ERROR("SDL_MaximizeWindow() error: {}", SDL_GetError());
            }
        }
        else {
            if (!SDL_SetWindowSize(m_window_handle, width, height)) {
                GC_ERROR("SDL_SetWindowSize() failed: {}", SDL_GetError());
            }
        }
    }

    /* Don't block until resize has finished. There is no need. */
    // if (!SDL_SyncWindow(m_window_handle)) {
    //     GC_ERROR("SDL_SyncWindow() timed out");
    // }
}

std::array<int, 2> Window::getSize() const
{
    int w{}, h{};
    if (!SDL_GetWindowSize(m_window_handle, &w, &h)) {
        GC_ERROR("SDL_GetWindowSize() error: ", SDL_GetError());
        return {1024, 768}; // return something reasonable. Not like this function will ever error anyway.
    }
    return {w, h};
}

bool Window::getIsFullscreen() const { return m_is_fullscreen; }

void Window::setIsResizable(bool resizable) { SDL_SetWindowResizable(m_window_handle, resizable); }

bool Window::getKeyDown(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::DOWN || state == ButtonState::JUST_PRESSED);
}

bool Window::getKeyUp(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::UP || state == ButtonState::JUST_RELEASED);
}

bool Window::getKeyPress(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::JUST_PRESSED);
}

bool Window::getKeyRelease(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::JUST_RELEASED);
}

bool Window::getButtonDown(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::DOWN || state == ButtonState::JUST_PRESSED);
}

bool Window::getButtonUp(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::UP || state == ButtonState::JUST_RELEASED);
}

bool Window::getButtonPress(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::JUST_PRESSED);
}

bool Window::getButtonRelease(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::JUST_RELEASED);
}

bool Window::getResizedFlag() const { return m_resized_flag; }

std::optional<SDL_DisplayMode> Window::findDisplayMode(int width, int height) const
{
    // Modes are more-or-less sorted best to worst, so return the first found matching mode
    for (const SDL_DisplayMode& mode : m_display_modes) {
        if (mode.w == width && mode.h == height) {
            return mode;
        }
    }
    return {};
}

} // namespace gc
