#pragma once

#include <memory>

/*
This is the root of the entire game.
Call App::instance() to initialise. App is shutdown at end of program.
*/

namespace gc {

class Logger; // forward-dec
class Jobs;   // forward-dec
class Content; // forward-dec

class App {

    // Lifetime must be explicitly controlled using initialise() and shutdown()
    static App* s_app;

    std::unique_ptr<Jobs> m_jobs;

    std::unique_ptr<Content> m_assets;

private:
    /* application lifetime is controlled by static variable 'instance' in instance() static method */
    App();
    ~App();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    static void initialise();
    static void shutdown();

    /* no nullptr checking is done here so ensure initialise() was called */
    static App& instance();

    /* Get Job instance (run functions in parallel) */
    Jobs& jobs();
};

} // namespace gc
