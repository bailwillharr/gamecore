#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_instance.h>
#include <gamecore/gc_vulkan_device.h>
#include <gamecore/gc_vulkan_allocator.h>

#include <SDL3/SDL_main.h>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    VkInstance instance = gc::vulkanInitialise();
    auto device_opt = gc::vulkanCreateDevice(instance);
    if (device_opt) {
        GC_TRACE("Vulkan device created!");
        gc::VulkanDevice& device = device_opt.value();

        auto alloc_opt = gc::vulkanAllocatorCreate(instance, device);
        if (alloc_opt) {
            GC_TRACE("Vulkan allocator created!");
            VmaAllocator alloc = alloc_opt.value();
            gc::vulkanAllocatorDestroy(alloc);
        }

        vkDestroyDevice(device.device, nullptr);
    }
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
