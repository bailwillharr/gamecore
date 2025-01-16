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
    //gc::app().window().setSize(1920, 1080, true);

    const uint64_t counts_per_second = SDL_GetPerformanceFrequency();
    uint64_t framecount = 0;
    const uint64_t counter_start = SDL_GetPerformanceCounter();

    while (!gc::app().window().shouldQuit()) {
        gc::app().window().processEvents();
        gc::app().vulkanRenderer().acquireAndPresent();
        ++framecount;
    }

    const uint64_t counter_end = SDL_GetPerformanceCounter();
    const double time_elapsed = static_cast<double>(counter_end - counter_start) / static_cast<double>(counts_per_second);
    const double fps = static_cast<double>(framecount) / time_elapsed;
    const std::string msg_string = std::format("Frames: {}, Seconds: {}, FPS: {}", framecount, time_elapsed, fps);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Stats", msg_string.c_str(), NULL);
    
    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
