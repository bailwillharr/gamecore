#include "gamecore/gc_window.h"

#include <span>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <tracy/Tracy.hpp>

#include <backends/imgui_impl_sdl3.h>

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

bool WindowState::getKeyDown(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::DOWN || state == ButtonState::JUST_PRESSED);
}

bool WindowState::getKeyUp(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::UP || state == ButtonState::JUST_RELEASED);
}

bool WindowState::getKeyPress(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::JUST_PRESSED);
}

bool WindowState::getKeyRelease(SDL_Scancode key) const
{
    const ButtonState state = m_keyboard_state[key];
    return (state == ButtonState::JUST_RELEASED);
}

bool WindowState::getButtonDown(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::DOWN || state == ButtonState::JUST_PRESSED);
}

bool WindowState::getButtonUp(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::UP || state == ButtonState::JUST_RELEASED);
}

bool WindowState::getButtonPress(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::JUST_PRESSED);
}

bool WindowState::getButtonRelease(MouseButton button) const
{
    const ButtonState state = m_mouse_button_state[static_cast<uint32_t>(button)];
    return (state == ButtonState::JUST_RELEASED);
}

const glm::vec2& WindowState::getMousePosition() const { return m_mouse_position; }

const glm::vec2& WindowState::getMousePositionNorm() const { return m_mouse_position_norm; }

const glm::vec2& WindowState::getMouseMotion() const { return m_mouse_motion; }

bool WindowState::getIsFullscreen() const { return m_is_fullscreen; }

bool WindowState::getResizedFlag() const { return m_resized_flag; }

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
    window_flags |= info.vulkan_support ? SDL_WINDOW_VULKAN : 0;
    m_window_handle = SDL_CreateWindow(INITIAL_TITLE, INITIAL_WIDTH, INITIAL_HEIGHT, window_flags);
    if (!m_window_handle) {
        GC_ERROR("SDL_CreateWindow() error: {}", SDL_GetError());
        abortGame("Failed to create window.");
    }

    m_state.m_window_size.x = INITIAL_WIDTH;
    m_state.m_window_size.y = INITIAL_HEIGHT;
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

const WindowState& Window::processEvents(const std::function<void(SDL_Event&)>& event_interceptor)
{
    ZoneScoped;

    resetKeyboardState(m_state.m_keyboard_state);
    resetMouseButtonState(m_state.m_mouse_button_state);
    m_state.m_mouse_motion = {};
    m_state.m_resized_flag = false;

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {

        if (event_interceptor) {
            event_interceptor(ev);
        }

        switch (ev.type) {
            case SDL_EVENT_QUIT:
                setQuitFlag();
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                m_state.m_window_size.x = static_cast<uint32_t>(ev.window.data1);
                m_state.m_window_size.y = static_cast<uint32_t>(ev.window.data2);
                m_state.m_mouse_position_norm.x = (2.0f * static_cast<float>(ev.motion.x) / static_cast<float>(m_state.m_window_size.x)) - 1.0f;
                m_state.m_mouse_position_norm.y = (-2.0f * static_cast<float>(ev.motion.y) / static_cast<float>(m_state.m_window_size.y)) + 1.0f;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                GC_TRACE("Window event: SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED");
                m_state.m_resized_flag = true;
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                m_state.m_is_fullscreen = true;
                break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                m_state.m_is_fullscreen = false;
                break;
            case SDL_EVENT_KEY_DOWN: {
                ButtonState& state = m_state.m_keyboard_state[ev.key.scancode];
                if (state == ButtonState::UP) {
                    state = ButtonState::JUST_PRESSED;
                }
            } break;
            case SDL_EVENT_KEY_UP: {
                ButtonState& state = m_state.m_keyboard_state[ev.key.scancode];
                if (state == ButtonState::DOWN) {
                    state = ButtonState::JUST_RELEASED;
                }
            } break;
            case SDL_EVENT_MOUSE_MOTION:
                m_state.m_mouse_position.x = ev.motion.x;
                m_state.m_mouse_position.y = ev.motion.y;
                m_state.m_mouse_position_norm.x = (2.0f * static_cast<float>(ev.motion.x) / static_cast<float>(m_state.m_window_size.x)) - 1.0f;
                m_state.m_mouse_position_norm.y = (-2.0f * static_cast<float>(ev.motion.y) / static_cast<float>(m_state.m_window_size.y)) + 1.0f;
                if (SDL_GetWindowRelativeMouseMode(m_window_handle)) {
                    m_state.m_mouse_motion.x = ev.motion.xrel;
                    m_state.m_mouse_motion.y = -ev.motion.yrel;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                ButtonState& state = m_state.m_mouse_button_state[ev.button.button - 1];
                if (state == ButtonState::UP) {
                    state = ButtonState::JUST_PRESSED;
                }
            } break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                ButtonState& state = m_state.m_mouse_button_state[ev.button.button - 1];
                if (state == ButtonState::DOWN) {
                    state = ButtonState::JUST_RELEASED;
                }
            } break;
            case SDL_EVENT_MOUSE_WHEEL:
                // handle mouse wheel
                break;
                // handle gamepad / joystick here
                // handle clipboard here
                // handle drag and drop here
                // handle audio device here
        }
    }
    return m_state;
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

    if (fullscreen) {
        const SDL_DisplayMode* mode = nullptr;
        const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window_handle);
        if (display) {
            if (width != 0 && height != 0) {
                // have to get list of available display modes
                SDL_DisplayMode** available_modes = SDL_GetFullscreenDisplayModes(display, nullptr);
                if (available_modes) {
                    for (int i = 0; available_modes[i] != nullptr; ++i) {
                        if (static_cast<uint32_t>(available_modes[i]->w) == width && static_cast<uint32_t>(available_modes[i]->h) == height) {
                            mode = available_modes[i];
                            break;
                        }
                    }
                    SDL_free(available_modes);
                }
                else {
                    GC_ERROR("SDL_GetFullscreenDisplayModes() error: {}", SDL_GetError());
                }
            }
            else {
                // Use desktop display mode
                mode = SDL_GetDesktopDisplayMode(display);
                if (!mode) {
                    GC_ERROR("SDL_GetDesktopDisplayMode() error: {}", SDL_GetError());
                }
            }
        }
        else {
            GC_ERROR("SDL_GetDisplayForWindow() error: {}", SDL_GetError());
        }
        if (!SDL_SetWindowFullscreenMode(m_window_handle, mode)) {
            GC_ERROR("SDL_SetWindowFullscreenMode() error: {}", SDL_GetError());
        }
        if (!SDL_SetWindowFullscreen(m_window_handle, true)) {
            GC_ERROR("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
        }
    }
    else { // regular windowed mode

        if (!SDL_SetWindowFullscreen(m_window_handle, false)) {
            GC_ERROR("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
        }
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

    if (!SDL_SyncWindow(m_window_handle)) {
        GC_ERROR("SDL_SyncWindow() timed out");
    }
}

void Window::setIsResizable(bool resizable) { SDL_SetWindowResizable(m_window_handle, resizable); }

} // namespace gc
