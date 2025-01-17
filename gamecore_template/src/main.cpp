#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_renderer.h>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::app().window().setTitle("Hello world!");
    gc::app().window().setWindowVisibility(true);
    gc::app().window().setIsResizable(true);
    // gc::app().window().setSize(1920, 1080, true);

    // use another render thread
    std::atomic<bool> game_running(true);
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();
    std::thread render_thread([&renderer, &game_running]() {
        uint64_t counter = SDL_GetPerformanceCounter();
        while (game_running.load()) {
            renderer.acquireAndPresent();
        }
        counter = SDL_GetPerformanceCounter() - counter;
        const uint64_t framecount = renderer.getFramecount();
        const double seconds = static_cast<double>(counter) / static_cast<double>(SDL_GetPerformanceFrequency());
        const double fps = static_cast<double>(framecount) / seconds;
        GC_INFO("Frames: {}, Seconds: {}, FPS: {}", framecount, seconds, fps);
    });

    while (!gc::app().window().shouldQuit()) {
        gc::app().window().processEvents();
    }

    game_running.store(false);
    render_thread.join();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
