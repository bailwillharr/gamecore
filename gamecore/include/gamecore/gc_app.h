#pragma once

#include <memory>
#include <thread>
#include <filesystem>
#include <string>

/*
This is the root of the entire game.
Is responsible for handling SDL initialisation and shutown.
Owns instances of game engine subsystems, such as the job system and content manager.
Call App::initialise() to initialise. Call shutdown() at end of program
*/

namespace gc {

class Jobs;          // forward-dec
class Content;       // forward-dec
class Window;        // forward-dec
class RenderBackend; // forward-dec
class DebugUI;       // forward-dec
class World;         // forward-dec

struct AppInitOptions {
    // None of these strings should have spaces.
    // These strings are copied.
    const char* name;
    const char* author;
    const char* version;
};

class App {

    // Lifetime must be explicitly controlled using initialise() and shutdown()
    static App* s_app;

    // Objects are destroyed in reverse order
    // Objects at the bottom of the list can safely access objects higher up in the list in their destructor
    std::unique_ptr<Jobs> m_jobs{};
    std::unique_ptr<Content> m_content{};
    std::unique_ptr<Window> m_window{};
    std::unique_ptr<RenderBackend> m_render_backend{};
    std::unique_ptr<DebugUI> m_debug_ui{};
    std::unique_ptr<World> m_world{};

    const std::thread::id m_main_thread_id{};
    std::filesystem::path m_save_directory{};

private:
    /* application lifetime is controlled by static variable 'instance' in instance() static method */
    explicit App(const AppInitOptions& options);
    ~App();

public:
    App(const App&) = delete;
    App(App&&) = delete;

    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    bool isMainThread() const;

    /* Access global engine components with these methods: */

    Jobs& jobs();
    Content& content();
    Window& window();
    RenderBackend& renderBackend();

    // Call before using any engine functionality (apart from logging)
    static void initialise(const AppInitOptions& options);
    static void shutdown();
    /* no nullptr checking is done here so ensure initialise() was called */
    static App& instance();

    void run();
};

// helper function to avoid having to write App::instance().whatever
inline App& app() { return App::instance(); }

} // namespace gc
