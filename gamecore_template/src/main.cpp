#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_instance.h>
#include <gamecore/gc_vulkan_device.h>

#include <SDL3/SDL_main.h>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    VkInstance instance = gc::vulkanInitialise();
    gc::vulkanCreateDevice(instance);
    vkDestroyInstance(instance, nullptr);

    gc::app().window().setTitle("Hello world!");
    if (!gc::app().window().setSize(256, 256, false)) {
        GC_ERROR("Failed to set window size");
    }
    gc::app().window().setWindowVisibility(true);

    while (!gc::app().window().shouldQuit()) {
        gc::app().window().processEvents();
    }

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
