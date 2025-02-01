#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_common.h>
#include <gamecore/gc_vulkan_renderer.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_content.h>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <tracy/Tracy.hpp>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();

    /* compile a pipeline rq */
    auto [my_pipeline, my_pipeline_layout] = renderer.createPipeline();
    vkDestroyPipeline(renderer.)

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    // use another render thread
    /*
    std::atomic<bool> game_running(true);
    std::atomic<uint64_t> sync_frame_count(0);
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();
    std::thread render_thread([&renderer, &game_running, &sync_frame_count]() {
        tracy::SetThreadName("Render Thread");

        while (game_running.load()) {


            sync_frame_count.fetch_add(1);
            sync_frame_count.notify_all();
            // The render thread can loop faster than the main thread.
            FrameMark;
        }
    });
    */

    while (!win.shouldQuit()) {
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

        // wait for renderer to finish rendering and increase the frame count
        /* {
            ZoneScopedNC("Wait for render thread", tracy::Color::Crimson);
            sync_frame_count.wait(sync_frame_count.load());
        }*/
    }

    // game_running.store(false);
    // render_thread.join();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
