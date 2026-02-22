#include "gamecore/gc_window.h"

#include <span>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_timer.h>

#include <tracy/Tracy.hpp>

#include <backends/imgui_impl_sdl3.h>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_threading.h"

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

const bool WindowState::getIsMouseCaptured() const { return m_mouse_captured; }

bool WindowState::getIsFullscreen() const { return m_is_fullscreen; }

bool WindowState::getResizedFlag() const { return m_resized_flag; }

const std::string& WindowState::getDragDropPath() const { return m_drag_drop_path; }

Window::Window(const WindowInitInfo& info)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        GC_ERROR("SDL_InitSubSystem() error: {}", SDL_GetError());
        abortGame("Failed to initialise SDL video subsystem.");
    }

    { // register events for mouse capture and mouse release
        uint32_t first_index = SDL_RegisterEvents(2);
        if (first_index == 0) {
            GC_ERROR("SDL_RegisterEvents() error");
            abortGame("Failed to register events with SDL");
        }
        m_mouse_capture_event_index = first_index;
        m_mouse_release_event_index = first_index + 1;
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

    m_window_id = SDL_GetWindowID(m_window_handle);
    if (!m_window_id) {
        GC_ERROR("SDL_GetWindowID() error: {}", SDL_GetError());
        abortGame("Failed to get SDL_WindowID");
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

const WindowState& Window::processEvents(void (*event_interceptor)(SDL_Event&))
{
    ZoneScoped;

    resetKeyboardState(m_state.m_keyboard_state);
    resetMouseButtonState(m_state.m_mouse_button_state);
    m_state.m_mouse_motion = {};
    m_state.m_resized_flag = false;
    m_state.m_drag_drop_path.clear();

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {

        if (event_interceptor) {
            event_interceptor(ev);
        }

        switch (ev.type) {
            case SDL_EVENT_QUIT: {
                m_should_quit = true;
            } break;
            case SDL_EVENT_WINDOW_RESIZED: {
                m_state.m_window_size.x = static_cast<uint32_t>(ev.window.data1);
                m_state.m_window_size.y = static_cast<uint32_t>(ev.window.data2);
                m_state.m_mouse_position_norm.x = (2.0f * static_cast<float>(ev.motion.x) / static_cast<float>(m_state.m_window_size.x)) - 1.0f;
                m_state.m_mouse_position_norm.y = (-2.0f * static_cast<float>(ev.motion.y) / static_cast<float>(m_state.m_window_size.y)) + 1.0f;
            } break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                m_state.m_resized_flag = true;
            } break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: {
                m_state.m_is_fullscreen = true;
            } break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
                m_state.m_is_fullscreen = false;
            } break;
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
            case SDL_EVENT_MOUSE_MOTION: {
                m_state.m_mouse_position.x = ev.motion.x;
                m_state.m_mouse_position.y = ev.motion.y;
                m_state.m_mouse_position_norm.x = (2.0f * static_cast<float>(ev.motion.x) / static_cast<float>(m_state.m_window_size.x)) - 1.0f;
                m_state.m_mouse_position_norm.y = (-2.0f * static_cast<float>(ev.motion.y) / static_cast<float>(m_state.m_window_size.y)) + 1.0f;
                if (SDL_GetWindowRelativeMouseMode(m_window_handle)) {
                    // Mouse motion events can occur multiple times per frame when FPS drops, these need to be accumulated.
                    m_state.m_mouse_motion.x += ev.motion.xrel;
                    m_state.m_mouse_motion.y += -ev.motion.yrel;
                }
            } break;
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
                // handle mouse wheel here
                // handle gamepad / joystick here
                // handle clipboard here
            case SDL_EVENT_DROP_FILE: {
                m_state.m_drag_drop_path = ev.drop.data;
            } break;
                // handle audio device here
            default: {
                if (ev.type == m_mouse_capture_event_index) {
                    m_state.m_mouse_captured = true;
                    if (!SDL_SetWindowRelativeMouseMode(m_window_handle, true)) {
                        GC_ERROR("SDL_SetWindowRelativeMouseMode() error: {}", SDL_GetError());
                    }
                }
                else if (ev.type == m_mouse_release_event_index) {
                    m_state.m_mouse_captured = false;
                    if (!SDL_SetWindowRelativeMouseMode(m_window_handle, false)) {
                        GC_ERROR("SDL_SetWindowRelativeMouseMode() error: {}", SDL_GetError());
                    }
                }
            } break;
        }
    }
    return m_state;
}

void Window::setWindowVisibility(bool visible)
{
    GC_ASSERT(isMainThread());
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

void Window::pushQuitEvent()
{
    SDL_Event ev{};
    ev.quit.type = SDL_EVENT_QUIT;
    ev.quit.timestamp = SDL_GetTicksNS();
    if (!SDL_PushEvent(&ev)) {
        GC_ERROR("SDL_PushEvent() error: {}", SDL_GetError());
        gc::abortGame("pushQuitEvent() error, aborting...");
    }
}

bool Window::shouldQuit() const
{
    GC_ASSERT(isMainThread());
    return m_should_quit;
}

void Window::setTitle(const std::string& title)
{
    GC_ASSERT(isMainThread());
    if (!SDL_SetWindowTitle(m_window_handle, title.c_str())) {
        GC_ERROR("SDL_SetWindowTitle() error: {}", SDL_GetError());
    }
}

void Window::setSize(uint32_t width, uint32_t height, bool fullscreen)
{
    GC_ASSERT(isMainThread());
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
            GC_ERROR("SDL_SetWindowFullscreen() error: {}", SDL_GetError());
        }
    }
    else { // regular windowed mode
        if (!SDL_SetWindowFullscreen(m_window_handle, false)) {
            GC_ERROR("SDL_SetWindowFullscreen() error: {}", SDL_GetError());
        }
        if (width == 0 || height == 0) {
            if (!SDL_MaximizeWindow(m_window_handle)) {
                GC_ERROR("SDL_MaximizeWindow() error: {}", SDL_GetError());
            }
        }
        else {
            if (!SDL_SetWindowSize(m_window_handle, width, height)) {
                GC_ERROR("SDL_SetWindowSize() error: {}", SDL_GetError());
            }
        }
    }
    if (!SDL_SyncWindow(m_window_handle)) {
        GC_ERROR("SDL_SyncWindow() timed out");
    }
}

void Window::setIsResizable(bool resizable)
{
    GC_ASSERT(isMainThread());
    if (!SDL_SetWindowResizable(m_window_handle, resizable)) {
        GC_ERROR("SDL_SetWindowResizable() error: {}", SDL_GetError());
    }
}

bool Window::getIsResizable() const { return SDL_GetWindowFlags(m_window_handle) & SDL_WINDOW_RESIZABLE; }

void Window::setMouseCaptured(bool captured)
{
    SDL_Event ev{};
    if (captured) {
        ev.type = m_mouse_capture_event_index;
    }
    else {
        ev.type = m_mouse_release_event_index;
    }
    ev.user.timestamp = SDL_GetTicksNS();
    ev.user.windowID = m_window_id;
    if (!SDL_PushEvent(&ev)) {
        GC_ERROR("SDL_PushEvent() error: {}", SDL_GetError());
        return;
    }
}

} // namespace gc
