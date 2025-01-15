#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_renderer.h>

#include <SDL3/SDL_main.h>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::app().window().setTitle("Hello world!");
    gc::app().window().setWindowVisibility(true);

    while (!gc::app().window().shouldQuit()) {
        gc::app().window().processEvents();
        gc::app().vulkanRenderer().acquireAndPresent();
    }

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
