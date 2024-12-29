#pragma once

#include <memory>

/*
This is the root of the entire game.
Call App::initialise() to initialise. Call shutdown() at end of program
*/

namespace gc {

class Logger; // forward-dec
class Jobs;   // forward-dec
class Content; // forward-dec

class App {

    // Lifetime must be explicitly controlled using initialise() and shutdown()
    static App* s_app;

    std::unique_ptr<Jobs> m_jobs{};
    std::unique_ptr<Content> m_content{};

private:
    /* application lifetime is controlled by static variable 'instance' in instance() static method */
    App();
    ~App();

    /* no nullptr checking is done here so ensure initialise() was called */
    static App& instance();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    static void initialise();
    static void shutdown();

    static Jobs& jobs();
    static Content& content();
};

} // namespace gc
