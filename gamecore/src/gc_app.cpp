#include "gamecore/gc_app.h"

#include <array>
#include <memory>
#include <thread>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_defines.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_jobs.h"
#include "gamecore/gc_content.h"

namespace gc {

// empty ptr, it is initialised manually in application
App* App::s_app = nullptr;

App::App()
{
    m_jobs = std::make_unique<Jobs>(std::thread::hardware_concurrency());
    m_content = std::make_unique<Content>();
    GC_TRACE("Initialised application");
}

App::~App()
{
    GC_TRACE("Shutting down application");
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
    if (!s_app) {
        abortGame("App::shutdown() called when App is not initialised!");
    }
    delete s_app;
    s_app = nullptr;
}

App& App::instance() { return *s_app; }

Jobs& App::jobs()
{
    App& inst = instance();
    GC_ASSERT(inst.m_jobs);
    return *inst.m_jobs;
}

Content& App::content()
{
    App& inst = instance();
    GC_ASSERT(inst.m_content);
    return *inst.m_content;
}

} // namespace gc
