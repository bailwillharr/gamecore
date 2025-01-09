#pragma once

#include <memory>
#include <thread>

/*
This is the root of the entire game.
Is responsible for handling SDL initialisation and shutown.
Owns instances of game engine subsystems, such as the job system and content manager.
Call App::initialise() to initialise. Call shutdown() at end of program
*/

namespace gc {

class Jobs;    // forward-dec
class Content; // forward-dec
class Window;  // forward-dec
class VulkanRenderer; // forward-dec

class App {

    // Lifetime must be explicitly controlled using initialise() and shutdown()
    static App* s_app;

    const std::thread::id m_main_thread_id{};

    std::unique_ptr<Jobs> m_jobs{};
    std::unique_ptr<Content> m_content{};
    std::unique_ptr<Window> m_window{};
    std::unique_ptr<VulkanRenderer> m_vulkan_renderer{};

private:
    /* application lifetime is controlled by static variable 'instance' in instance() static method */
    App();
    ~App();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    bool isMainThread() const;

    Jobs& jobs();
    Content& content();
    Window& window();

    // Call before using any engine functionality (apart from logging)
    // This function should be very fast and only bring gc::App to a usable state
    static void initialise();
    static void shutdown();
    /* no nullptr checking is done here so ensure initialise() was called */
    static App& instance();

};

// helper function to avoid having to write App::instance().whatever
inline App& app() { return App::instance(); }

} // namespace gc
