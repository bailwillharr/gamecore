#include "gamecore/gc_window.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"

namespace gc {

static constexpr const char* INITIAL_TITLE = "Gamecore Game Window";
static constexpr int INITIAL_WIDTH = 1024;
static constexpr int INITIAL_HEIGHT = 768;

Window::Window(const WindowInitInfo& info) : m_should_quit(false)
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
    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                setQuitFlag();
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                // handle keyboard
                break;
            case SDL_EVENT_MOUSE_MOTION:
                // handle mouse motion
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
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

bool Window::setSize(int width, int height, bool fullscreen)
{
    GC_ASSERT(width > 0);
    GC_ASSERT(height > 0);

    bool success = true;

    if (!SDL_SetWindowFullscreen(m_window_handle, fullscreen)) {
        GC_ERROR("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
        success = false;
    }

    if (fullscreen) {
        auto mode = findDisplayMode(width, height);
        const SDL_DisplayMode* mode_ptr{}; // can be null for windowed fullscreen
        if (mode) {
            mode_ptr = &mode.value();
        }
        if (!SDL_SetWindowFullscreenMode(m_window_handle, mode_ptr)) {
            GC_ERROR("SDL_SetWindowFullscreenMode() error: {}", SDL_GetError());
            success = false;
        }
    }
    else { // regular windowed mode
        if (!SDL_SetWindowSize(m_window_handle, width, height)) {
            GC_ERROR("SDL_SetWindowSize() failed: {}", SDL_GetError());
            success = false;
        }
    }

    SDL_ClearError(); // not sure if SDL_SyncWindow() always calls SDL_SetError()
    if (!SDL_SyncWindow(m_window_handle)) {
        GC_ERROR("SDL_SyncWindow() error: {}", SDL_GetError());
        success = false;
    }
    return success;
}

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