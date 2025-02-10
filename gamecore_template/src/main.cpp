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

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <tracy/Tracy.hpp>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();
    const gc::VulkanDevice& device = renderer.getDevice();
    const gc::VulkanSwapchain& swapchain = renderer.getSwapchain();

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    gc::app().jobs().execute([&pipeline, &pipeline_layout]() {
        std::tie(pipeline, pipeline_layout) = gc::createPipeline();
        SDL_Delay(1000);
    });


    std::array<VkCommandPool, gc::VULKAN_FRAMES_IN_FLIGHT> command_pools{};
    std::array<VkCommandBuffer, gc::VULKAN_FRAMES_IN_FLIGHT> command_buffers{};

    for (int i = 0; i < gc::VULKAN_FRAMES_IN_FLIGHT; ++i) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = 0;
        pool_info.queueFamilyIndex = device.getMainQueue().queue_family_index;
        GC_CHECKVK(vkCreateCommandPool(device.getDevice(), &pool_info, nullptr, &command_pools[i]));

        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = command_pools[i];
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        cmdAllocInfo.commandBufferCount = 1;
        GC_CHECKVK(vkAllocateCommandBuffers(device.getDevice(), &cmdAllocInfo, &command_buffers[i]));
    }

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

        renderer.waitForRenderFinished();

        std::vector<VkCommandBuffer> bufs{};
        if (!gc::app().jobs().isBusy()) {

            // reset command buffer
            GC_CHECKVK(vkResetCommandPool(device.getDevice(), command_pools[renderer.getFrameInFlightIndex()], 0));

            const VkCommandBuffer cmd = command_buffers[renderer.getFrameInFlightIndex()];
            const VkFormat color_attachment_format = swapchain.getSurfaceFormat().format;

            VkCommandBufferInheritanceRenderingInfo inheritance_rendering_info{};
            inheritance_rendering_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            inheritance_rendering_info.pNext = nullptr;
            inheritance_rendering_info.flags =
                0; // https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdExecuteCommands.html#VUID-vkCmdExecuteCommands-flags-06026
            inheritance_rendering_info.viewMask = 0;
            inheritance_rendering_info.colorAttachmentCount = 1;
            inheritance_rendering_info.pColorAttachmentFormats = &color_attachment_format;
            inheritance_rendering_info.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
            inheritance_rendering_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
            inheritance_rendering_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkCommandBufferInheritanceInfo inheritance_info{};
            inheritance_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inheritance_info.pNext = &inheritance_rendering_info;
            inheritance_info.renderPass = VK_NULL_HANDLE;  // ignored with dynamic rendering
            inheritance_info.subpass = 0;                  // ignored with dynamic rendering
            inheritance_info.framebuffer = VK_NULL_HANDLE; // ignored with dynamic rendering
            inheritance_info.occlusionQueryEnable = VK_FALSE;
            inheritance_info.queryFlags = 0;
            inheritance_info.pipelineStatistics = 0;

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            begin_info.pInheritanceInfo = &inheritance_info;
            GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

            // we are in render pass here
            // dynamic state cannot be set in primary command buffers as state does not remain between cmd bufs
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapchain.getExtent().width);
            viewport.height = static_cast<float>(swapchain.getExtent().height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.offset.x = 0;
            scissor.offset.y = 0;
            scissor.extent = swapchain.getExtent();
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            float push_val = 1.0f;
            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &push_val);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            GC_CHECKVK(vkEndCommandBuffer(cmd));

            bufs.push_back(cmd);
        }

        renderer.acquireAndPresent(bufs);

        FrameMark;
    }

    // game_running.store(false);
    // render_thread.join();

    renderer.waitIdle();

    for (int i = 0; i < gc::VULKAN_FRAMES_IN_FLIGHT; ++i) {
        vkFreeCommandBuffers(device.getDevice(), command_pools[i], 1, &command_buffers[i]);
        vkDestroyCommandPool(device.getDevice(), command_pools[i], nullptr);
    }

    if (pipeline != VK_NULL_HANDLE) {
        gc::destroyPipeline(pipeline, pipeline_layout);
    }

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
