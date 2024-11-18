#include "gamecore/gc_app.h"

#include <array>
#include <memory>
#include <thread>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_defines.h"
#include "gamecore/gc_logger_null.h"
#include "gamecore/gc_logger_spdlog.h"
#include "gamecore/gc_jobs.h"

namespace gc {

App::App()
    :
#if GC_LOGGER == GC_LOGGER_SPDLOG
      m_logger(std::make_unique<LoggerSpdlog>()),
#else
      m_logger(std::make_unique<LoggerNull>()),
#endif
      m_jobs(std::make_unique<Jobs>(std::thread::hardware_concurrency()))
{
}

App::~App()
{
    // job threads should be stopped here because otherwise other engine systems may shut down while still in use by those threads.
    // Ideally, job system shouldn't be busy at this point anyway since jobs shouldn't be left running.
    if (jobs().isBusy()) {
        logger().error("Jobs were still running at time of application shutdown!");
        jobs().wait();
    }
}

App& App::instance()
{
    static App app;
    return app;
}

Logger& App::logger()
{
    GC_ASSERT_NOLOG(m_logger);
    return *m_logger;
}

void App::setLogger(std::unique_ptr<Logger>&& logger) { m_logger = std::move(logger); }

Jobs& App::jobs()
{
    GC_ASSERT(m_jobs);
    return *m_jobs;
}

} // namespace gc
