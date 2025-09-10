#include "gamecore/gc_app.h"

#include <memory>
#include <thread>
#include <string>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_filesystem.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_assert.h"
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

namespace gc {

// empty ptr, it is initialised manually in application
App* App::s_app = nullptr;

App::App(const AppInitOptions& options)
    : m_main_thread_id(std::this_thread::get_id()),
      m_performance_counter_frequency(SDL_GetPerformanceFrequency()),
      m_performance_counter_init(SDL_GetPerformanceCounter())
{
    // Setup app metadata for SDL
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

    // Get save directory (In $XDG_DATA_HOME on Linux and in %appdata% on Windows)
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

bool App::isMainThread() const { return std::this_thread::get_id() == m_main_thread_id; }

uint64_t App::getNanos() const
{
    // billion / frequency must be in brackets to avoid overflow
    uint64_t value = (SDL_GetPerformanceCounter() - m_performance_counter_init) * (static_cast<uint64_t>(1'000'000'000) / m_performance_counter_frequency);
    return value;
}

void App::initialise(const AppInitOptions& options)
{
    if (s_app) {
        abortGame("App::initialise() called when App is already initialised!");
    }
    s_app = new App(options);
}

void App::shutdown()
{
    if (s_app) {
        delete s_app;
        s_app = nullptr;
        SDL_Quit();
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

    m_last_frame_begin_stamp = getNanos() - 1'000'000; // treat the first delta time as 1 ms

    auto pipeline = renderBackend().createPipeline(content().loadAsset(strToName("vert_spv")), content().loadAsset(strToName("frag_spv")));
    WorldDrawData world_draw_data;
    world_draw_data.setPipeline(&pipeline);

    while (!window().shouldQuit()) {

        const auto frame_begin_stamp = getNanos();
        // do not pass delta-times close to zero or weird things can happen
        const auto dt = std::max((frame_begin_stamp - m_last_frame_begin_stamp) / 1e9, 1e-4); // min 0.1ms
        if (dt > 10.0) {
            GC_WARN("Abnormal delta time: {}", dt);
            GC_WARN("  frame_begin_stamp: {}", frame_begin_stamp);
            GC_WARN("  m_last_frame_begin_stamp: {}", m_last_frame_begin_stamp);
            GC_WARN("  m_performance_counter_frequency: {}", m_performance_counter_frequency);
            GC_WARN("  m_performance_counter_init: {}", m_performance_counter_init);
            GC_WARN("  getNanos() now: {}", getNanos());
            GC_WARN("  SDL_GetPerformanceCounter(): {}", SDL_GetPerformanceCounter());
        }

        window().processEvents();

        {
            ZoneScopedN("UI Logic");
            if (window().getKeyDown(SDL_SCANCODE_ESCAPE)) {
                window().setQuitFlag();
            }
            if (window().getKeyPress(SDL_SCANCODE_F11)) {
                window().setSize(0, 0, !window().getIsFullscreen());
            }
            if (window().getKeyPress(SDL_SCANCODE_F10)) {
                m_debug_ui->active = !m_debug_ui->active;
            }
            if (window().getButtonPress(gc::MouseButton::X1)) {
                // show/hide mouse
                if (!SDL_SetWindowRelativeMouseMode(window().getHandle(), !SDL_GetWindowRelativeMouseMode(window().getHandle()))) {
                    GC_ERROR("SDL_SetWindowRelativeMouseMode() error: {}", SDL_GetError());
                }
            }
            if (window().getKeyPress(SDL_SCANCODE_T)) {
                if (auto comp = m_world->getComponent<TransformComponent>(0)) {
                    comp->setPosition({1.0f, 1.0f, 1.0f});
                }
            }
        }

        m_world->update(dt);

        m_debug_ui->update(dt);

        world_draw_data.reset();
        for (const auto& cube_matrix : world().getSystem<CubeSystem>().getCubeTransforms()) {
            world_draw_data.drawCube(cube_matrix);
        }

        renderBackend().submitFrame(window().getResizedFlag(), world_draw_data);

        renderBackend().cleanupGPUResources();

        renderBackend().waitForFrameReady(); // reduces latency

        m_last_frame_begin_stamp = frame_begin_stamp;

        FrameMark;
    }
    GC_TRACE("Quitting...");
}

} // namespace gc
