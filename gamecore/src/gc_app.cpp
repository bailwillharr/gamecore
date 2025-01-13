#include "gamecore/gc_app.h"

#include <array>
#include <memory>
#include <thread>

#include <SDL3/SDL_init.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_defines.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_jobs.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_window.h"
#include "gamecore/gc_vulkan_renderer.h"

namespace gc {

// empty ptr, it is initialised manually in application
App* App::s_app = nullptr;

App::App() : m_main_thread_id(std::this_thread::get_id())
{
    m_jobs = std::make_unique<Jobs>(std::thread::hardware_concurrency());
    m_content = std::make_unique<Content>();

    // Setup app metadata for SDL
    {
        bool set_prop_success = true;
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "Gamecore Game Engine");
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, "v0.0.0");
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, "bailwillharr.gamecore");
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "Bailey Harrison");
        set_prop_success &= set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING, "Copyright (c) 2024 Bailey Harrison");
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING, "https://github.com/bailwillharr/gamecore");
        set_prop_success &= SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");
        if (!set_prop_success) {
            // not a big deal if these fail
            GC_WARN("Failed setting one or more SDL App Metadata properties");
        }
    }

    WindowInitInfo window_init_info{};
    window_init_info.load_vulkan = true;
    window_init_info.resizable = false;
    m_window = std::make_unique<Window>(window_init_info);

    m_vulkan_renderer = std::make_unique<VulkanRenderer>(m_window->getHandle());

    GC_TRACE("Initialised application");
}

App::~App()
{
    GC_TRACE("Destroying application...");
    // job threads should be stopped here because otherwise other engine systems may shut down while still in use by those threads.
    // Ideally, job system shouldn't be busy at this point anyway since jobs shouldn't be left running.
    if (jobs().isBusy()) {
        GC_ERROR("Jobs were still running at time of application shutdown!");
        jobs().wait();
    }
}

void App::initialise()
{
    if (s_app) {
        abortGame("App::initialise() called when App is already initialised!");
    }
    s_app = new App;
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

bool App::isMainThread() const { return std::this_thread::get_id() == m_main_thread_id; }

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

VulkanRenderer& App::vulkanRenderer()
{
    GC_ASSERT(m_vulkan_renderer);
    return *m_vulkan_renderer;
}

} // namespace gc
