#pragma once

#include <memory>

/*
This is the root of the entire game.
Call App::instance() to initialise. App is shutdown at end of program.
*/

namespace gc {

class Logger; // forward-dec
class Jobs;   // forward-dec

class App {

    /* Default instance is LoggerNull which does nothing (logging off) */
    std::unique_ptr<Logger> m_logger;

    std::unique_ptr<Jobs> m_jobs;

private:
    /* application lifetime is controlled by static variable 'instance' in instance() static method */
    App();
    ~App();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    /* The app is created on first call to this method. */
    /* App is destroyed at the end of the program */
    static App& instance();

    /* Get logger instance. */
    /* Logger is internally synchronised so logging can be performed from any thread. */
    Logger& logger();

    /* Set the logger to use. */
    /* Ensure that no other threads are running that ever use the logger() reference. */
    /* This will invalidate any existing logger() reference. */
    void setLogger(std::unique_ptr<Logger>&& logger);

    /* Get Job instance (run functions in parallel) */
    Jobs& jobs();
};

} // namespace gc
