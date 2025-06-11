#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_common.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_vulkan_pipeline.h>
#include <gamecore/gc_vulkan_allocator.h>
#include <gamecore/gc_vulkan_swapchain.h>
#include <gamecore/gc_stopwatch.h>

#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>
#include <glm.hpp>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <array>
#include <string>
#include <vector>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::RenderBackend& renderer = gc::app().vulkanRenderer();

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    while (!win.shouldQuit()) {

        win.processEvents();

        {
            ZoneScopedN("UI Logic");
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
        }

        renderer.renderFrame();

        FrameMark;
    }

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
