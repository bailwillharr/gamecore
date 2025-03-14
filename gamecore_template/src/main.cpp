#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_common.h>
#include <gamecore/gc_vulkan_renderer.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_vulkan_pipeline.h>

#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>
#include <glm.hpp>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <tracy/Tracy.hpp>


int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    const uint64_t counter_freq = SDL_GetPerformanceFrequency();
    uint64_t frame_start_time = SDL_GetPerformanceCounter();
    uint64_t last_frame_start_time = frame_start_time - counter_freq * 16 / 1000; // first delta time as 16 ms
    double elapsed_time = 0.0f;
    while (!win.shouldQuit()) {

        const double delta_time = static_cast<double>(frame_start_time - last_frame_start_time) / static_cast<double>(counter_freq);
        elapsed_time += delta_time;
        last_frame_start_time = frame_start_time;
        frame_start_time = SDL_GetPerformanceCounter();

        renderer.waitForPresentFinished();

        win.processEvents();

        if (win.getKeyDown(SDL_SCANCODE_ESCAPE)) {
            win.setQuitFlag();
        }
        if (win.getKeyPress(SDL_SCANCODE_F11)) {
            if (win.getIsFullscreen()) {
                win.setSize(0, 0, false);
            }
            else {
                win.setSize(0, 0, true);
            }
        }
        if (win.getButtonPress(gc::MouseButton::X1)) {
            // show/hide mouse
            if (!SDL_SetWindowRelativeMouseMode(win.getHandle(), !SDL_GetWindowRelativeMouseMode(win.getHandle()))) {
                GC_ERROR("SDL_SetWindowRelativeMouseMode() error: {}", SDL_GetError());
            }
        }

        renderer.acquireAndPresent();

        FrameMark;
    }

    renderer.waitIdle();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
