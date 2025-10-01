#include "gamecore/gc_app.h"

#include <memory>
#include <thread>
#include <string>
#include <numeric>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_filesystem.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_threading.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_jobs.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_window.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_debug_ui.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_cube_system.h"
#include "gamecore/gc_stopwatch.h"
#include "gamecore/gc_frame_state.h"

namespace gc {

// empty ptr, it is initialised manually in application
App* App::s_app = nullptr;

App::App(const AppInitOptions& options)
{

    /* Get save directory(In $XDG_DATA_HOME on Linux and in % appdata % on Windows) */
    const char* user_dir = SDL_GetPrefPath(options.author.c_str(), options.name.c_str());
    if (user_dir) {
        m_save_directory = std::filesystem::path(user_dir);
        SDL_free(const_cast<char*>(user_dir));
        GC_INFO("Save directory: {}", m_save_directory.string());
    }
    else {
        GC_ERROR("SDL_GetPrefPath() error: {}", SDL_GetError());
        GC_ERROR("Failed to get save directory! Falling back to current working directory.");
        m_save_directory = std::filesystem::current_path();
    }

    Logger::instance().setLogFile(m_save_directory / "logfile.txt");

    GC_INFO("STARTING GAME");

    /* Register some information for the program */
    {
        bool set_prop_success = true;
        // These functions copy the strings so they do not need to be saved
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, options.name.c_str());
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, options.version.c_str());
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, std::format("{}.{}", options.author, options.name).c_str());
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");
        if (!set_prop_success) {
            // not a big deal if these fail
            GC_WARN("Failed setting one or more SDL App Metadata properties");
        }
    }

    /* SUBSYSTEM INITIALISATION */

    m_jobs = std::make_unique<Jobs>(std::thread::hardware_concurrency());
    m_content = std::make_unique<Content>();
    WindowInitInfo window_init_info{};
    window_init_info.vulkan_support = true;
    window_init_info.resizable = false;
    m_window = std::make_unique<Window>(window_init_info);
    m_render_backend = std::make_unique<RenderBackend>(m_window->getHandle());
    m_debug_ui = std::make_unique<DebugUI>(m_window->getHandle(), m_render_backend->getInfo(), m_save_directory / "imgui.ini");
    m_world = std::make_unique<World>();

    GC_TRACE("Initialised Application");
}

App::~App()
{
    GC_TRACE("Destroying Application...");

    m_render_backend->waitIdle();

    // job threads should be stopped here because otherwise other engine systems may shut down while still in use by those threads.
    // Ideally, job system shouldn't be busy at this point anyway since jobs shouldn't be left running.
    if (jobs().isBusy()) {
        GC_ERROR("Jobs were still running at time of application shutdown!");
        jobs().wait();
    }
}

void App::initialise(const AppInitOptions& options)
{
    if (s_app) {
        abortGame("App::initialise() called when App is already initialised!");
    }
    isMainThread(); // first call to this function assigns the main thread
    s_app = new App(options);
}

void App::shutdown()
{
    if (s_app) {
        delete s_app;
        s_app = nullptr;
        SDL_Quit();
        GC_INFO("SHUT DOWN GAME");
    }
    else {
        abortGame("App::shutdown() called when App is already shutdown!");
    }
}

App& App::instance() { return *s_app; }

Jobs& App::jobs()
{
    GC_ASSERT(m_jobs);
    return *m_jobs;
}

Content& App::content()
{
    GC_ASSERT(m_content);
    return *m_content;
}

Window& App::window()
{
    GC_ASSERT(m_window);
    return *m_window;
}

RenderBackend& App::renderBackend()
{
    GC_ASSERT(m_render_backend);
    return *m_render_backend;
}

DebugUI& App::debugUI()
{
    GC_ASSERT(m_debug_ui);
    return *m_debug_ui;
}

World& App::world()
{
    GC_ASSERT(m_world);
    return *m_world;
}

void App::run()
{
    GC_TRACE("Starting game loop...");

    FrameState frame_state{};

    uint64_t frame_count{};
    std::array<double, 20> delta_times{};
    uint64_t frame_begin_stamp = SDL_GetTicksNS() - 16'666'667LL; // set first delta time to something reasonable
    while (!window().shouldQuit()) {
        Logger::instance().incrementFrameNumber();

        const uint64_t last_frame_begin_stamp = frame_begin_stamp;
        frame_begin_stamp = SDL_GetTicksNS();

        frame_state.delta_time = static_cast<double>(frame_begin_stamp - last_frame_begin_stamp) * 1e-9;
        delta_times[frame_count % delta_times.size()] = frame_state.delta_time;
        frame_state.average_frame_time = std::accumulate(delta_times.cbegin(), delta_times.cend(), 0.0) / static_cast<double>(delta_times.size());
        frame_state.window_state = &window().processEvents(DebugUI::windowEventInterceptor);
         
        {
            ZoneScopedN("UI Logic");
            if (frame_state.window_state->getKeyDown(SDL_SCANCODE_ESCAPE)) {
                window().pushQuitEvent();
            }
            if (frame_state.window_state->getKeyPress(SDL_SCANCODE_F11)) {
                window().setSize(0, 0, !frame_state.window_state->getIsFullscreen());
            }
            if (frame_state.window_state->getKeyPress(SDL_SCANCODE_F10)) {
                m_debug_ui->active = !m_debug_ui->active;
                window().setMouseCaptured(!m_debug_ui->active);
            }
        }

        m_world->update(frame_state);

        m_debug_ui->update(frame_state);

        renderBackend().submitFrame(frame_state.window_state->getResizedFlag(), frame_state.draw_data);
        frame_state.draw_data.reset();
        renderBackend().cleanupGPUResources();

        ++frame_count;
        FrameMark;
    }
    GC_TRACE("Quitting...");
}

} // namespace gc
