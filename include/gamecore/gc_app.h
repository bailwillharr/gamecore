#pragma once

#include <memory>

/*
This is the root of the entire game.
Call init() to initialise and call shutdown() at the end.
*/

namespace gc {

class Logger; // forward-dec

class App {
    std::unique_ptr<Logger> m_logger;

private:
    App();

    ~App();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    static App& instance();

    Logger& logger();
};

} // namespace gc
