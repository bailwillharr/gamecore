#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

int main(int argc, char* argv[])
{
    gc::App::initialise();
    gc::app().window().setTitle("Hello world!");
    gc::app().window().setSize(1280, 1024, false);
    gc::app().window().setWindowVisibility(true);

    const uint64_t counts_per_second = SDL_GetPerformanceFrequency();
    uint64_t framecount = 0;
    const uint64_t counter_start = SDL_GetPerformanceCounter();
    while (!gc::app().window().shouldQuit()) {
        gc::app().window().processEvents();
        ++framecount;
    }
    const uint64_t counter_end = SDL_GetPerformanceCounter();
    const double time_elapsed = static_cast<double>(counter_end - counter_start) / static_cast<double>(counts_per_second);
    const double fps = static_cast<double>(framecount) / time_elapsed;
    const std::string msg_string = std::format("Frames: {}, Seconds: {}, FPS: {}", framecount, time_elapsed, fps);
    GC_INFO("{}", msg_string);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Box", msg_string.c_str(), NULL);
    gc::App::shutdown();
    return 0;
}