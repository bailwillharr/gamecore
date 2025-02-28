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

#include <asio.hpp>

#include <tracy/Tracy.hpp>

static float x_pos = 0.0f, y_pos = 0.0f;
static bool remote_sent_quit = false;

static asio::awaitable<void> echo()
{
    constexpr asio::ip::port_type SERVER_PORT = 1234;

    /* shortcut to use error code with co_await */
    static const auto ec_awaitable = asio::as_tuple(asio::use_awaitable);

    /* get io_context for this coroutine (passed to co_spawn) */
    const asio::any_io_executor ctx = co_await asio::this_coro::executor;

    /* Get endpoint for the server (bound to 0.0.0.0 port 1234) */
    const asio::ip::tcp::endpoint server_endpoint(asio::ip::tcp::v4(), SERVER_PORT);

    for (;;) {
        /* acceptor object can asynchronously wait for a client to connect */
        asio::ip::tcp::acceptor acceptor(ctx, server_endpoint);

        GC_INFO("Waiting for connection...");

        /* returns a socket to communicate with a client */
        auto [ec, sock] = co_await acceptor.async_accept(ctx, ec_awaitable);
        if (ec) {
            gc::abortGame("acceptor.async_accept() error: {}", ec.message());
        }

        GC_INFO("Remote connected.");

        for (;;) {
            std::array<char, 512> buf{};
            auto [ec2, sz] = co_await sock.async_read_some(asio::buffer(buf.data(), buf.size()), ec_awaitable);
            if (ec2 == asio::error::eof) {
                GC_INFO("Remote disconnected.");
                break;
            }
            else if (ec2) {
                gc::abortGame("sock.async_read_some() error: {}", ec2.message());
            }

            for (char c : std::span(buf.begin(), buf.begin() + sz)) {
                switch (tolower(c)) {
                    case 'w':
                        y_pos += 0.1f;
                        break;
                    case 'a':
                        x_pos -= 0.1f;
                        break;
                    case 's':
                        y_pos -= 0.1f;
                        break;
                    case 'd':
                        x_pos += 0.1f;
                        break;
                    case 'q':
                        remote_sent_quit = true;
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

class TestClass {
public:
    TestClass() { printf("Constructed TestClass\n"); }
    ~TestClass() { printf("Destroying TestClass\n"); }
};

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();
    const gc::VulkanDevice& device = renderer.getDevice();
    const gc::VulkanSwapchain& swapchain = renderer.getSwapchain();

    asio::io_context ctx;
    co_spawn(ctx, echo(), asio::detached);

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
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
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

    const uint64_t counter_freq = SDL_GetPerformanceFrequency();

    uint64_t frame_start_time = SDL_GetPerformanceCounter();
    uint64_t last_frame_start_time = frame_start_time - counter_freq * 16 / 1000; // first delta time as 16 ms
    double elapsed_time = 0.0f;

    while (!win.shouldQuit()) {

        const double delta_time = static_cast<double>(frame_start_time - last_frame_start_time) / static_cast<double>(counter_freq);
        elapsed_time += delta_time;
        last_frame_start_time = frame_start_time;
        frame_start_time = SDL_GetPerformanceCounter();

        renderer.waitForRenderFinished();

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

        ctx.poll();

        if (remote_sent_quit) {
            win.setQuitFlag();
        }

        std::vector<VkCommandBuffer> bufs{};
        if (!gc::app().jobs().isBusy()) {
            ZoneScopedN("Record triangle command buffer");

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
            inheritance_rendering_info.depthAttachmentFormat = renderer.getDepthStencilFormat();
            inheritance_rendering_info.stencilAttachmentFormat = inheritance_rendering_info.depthAttachmentFormat;
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
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
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

			const glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
			const glm::vec3 position{ 0.0f, 0.0f, glm::sin(elapsed_time) * 10.0f };
			glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
			scale *= 0.2f;

			// apply rotation
			glm::mat4 world_transform = glm::mat4_cast(rotation);
			// apply position
			world_transform[3][0] = position.x;
			world_transform[3][1] = position.y;
			world_transform[3][1] = position.z;
			// apply scale
			world_transform = glm::scale(world_transform, scale);

			struct PushConstants {
				glm::mat4 world_transform;
				glm::mat4 projection;
			} push_constants{};

			push_constants.world_transform = world_transform;
			push_constants.projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 50.0f);

            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               static_cast<uint32_t>(sizeof(PushConstants)), &push_constants);

            vkCmdDraw(cmd, 36, 1, 0, 0);

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
