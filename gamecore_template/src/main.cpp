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

    VkImage my_image = VK_NULL_HANDLE;
    VmaAllocation my_image_allocation{};
    VkImageView my_image_view = VK_NULL_HANDLE;
    {
        /* create a texture */
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = nullptr;
        image_info.flags = 0;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
        image_info.extent.width = 8;
        image_info.extent.height = 8;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.queueFamilyIndexCount = 0;     // ignored
        image_info.pQueueFamilyIndices = nullptr; // ingored
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_create_info{};
        alloc_create_info.flags = 0;
        alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc_create_info.priority = 1.0f;
        GC_CHECKVK(vmaCreateImage(renderer.getAllocator(), &image_info, &alloc_create_info, &my_image, &my_image_allocation, nullptr));

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = nullptr;
        view_info.flags = 0;
        view_info.image = my_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        GC_CHECKVK(vkCreateImageView(renderer.getDevice().getDevice(), &view_info, nullptr, &my_image_view));

        const size_t buffer_size = static_cast<size_t>(image_info.extent.width) * static_cast<size_t>(image_info.extent.height) * 4;

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_buffer_allocation{};
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo buffer_alloc_create_info{};
        buffer_alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_alloc_create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        GC_CHECKVK(vmaCreateBuffer(renderer.getAllocator(), &buffer_info, &buffer_alloc_create_info, &staging_buffer, &staging_buffer_allocation, nullptr));

        std::vector<glm::u8vec4> source(buffer_size / 4);
        for (uint32_t y = 0; y < image_info.extent.height; ++y) {
            for (uint32_t x = 0; x < image_info.extent.width; ++x) {
                source[y * image_info.extent.width + x].r = ((x + y) % 2) * 255;
                source[y * image_info.extent.width + x].g = 0;
                source[y * image_info.extent.width + x].b = ((x + y) % 2) * 255;
                source[y * image_info.extent.width + x].a = 255;
            }
        }

        void* dest = nullptr;
        GC_CHECKVK(vmaMapMemory(renderer.getAllocator(), staging_buffer_allocation, &dest));

        memcpy(dest, source.data(), buffer_size);

        vmaUnmapMemory(renderer.getAllocator(), staging_buffer_allocation);

        VkCommandPool command_pool{};

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = device.getMainQueue().queue_family_index;
        GC_CHECKVK(vkCreateCommandPool(device.getDevice(), &pool_info, nullptr, &command_pool));

        VkCommandBuffer cmd{};

        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = command_pool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        GC_CHECKVK(vkAllocateCommandBuffers(device.getDevice(), &cmdAllocInfo, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_info.pInheritanceInfo = nullptr;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = my_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        {
            VkBufferImageCopy2 region{};
            region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            region.imageOffset.z = 0;
            region.imageExtent = image_info.extent;
            VkCopyBufferToImageInfo2 copy_info{};
            copy_info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
            copy_info.srcBuffer = staging_buffer;
            copy_info.dstImage = my_image;
            copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copy_info.regionCount = 1;
            copy_info.pRegions = &region;
            vkCmdCopyBufferToImage2(cmd, &copy_info);
        }

        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = my_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        GC_CHECKVK(vkEndCommandBuffer(cmd));

        VkFence my_fence{};

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        GC_CHECKVK(vkCreateFence(device.getDevice(), &fence_info, nullptr, &my_fence));

        VkCommandBufferSubmitInfo command_submit_info{};
        command_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_submit_info.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_submit_info;
        vkQueueSubmit2(device.getMainQueue().queue, 1, &submit, my_fence);

        GC_CHECKVK(vkWaitForFences(device.getDevice(), 1, &my_fence, VK_TRUE, UINT64_MAX));

        vkDestroyFence(device.getDevice(), my_fence, nullptr);
        vkDestroyCommandPool(device.getDevice(), command_pool, nullptr);
        vmaDestroyBuffer(renderer.getAllocator(), staging_buffer, staging_buffer_allocation);
    }

    VkSampler my_sampler{};
    {
        // create sampler
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.mipLodBias = 0.0f;
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy = device.getProperties().props.properties.limits.maxSamplerAnisotropy;
        sampler_info.compareEnable = VK_FALSE;
        sampler_info.unnormalizedCoordinates = VK_FALSE;
        GC_CHECKVK(vkCreateSampler(device.getDevice(), &sampler_info, nullptr, &my_sampler));
    }

    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    {
        // create descriptor set layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = nullptr;
        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info{};
        descriptor_set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_set_layout_info.bindingCount = 1;
        descriptor_set_layout_info.pBindings = &binding;
        GC_CHECKVK(vkCreateDescriptorSetLayout(device.getDevice(), &descriptor_set_layout_info, nullptr, &set_layout));

        // create descriptor pool
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        GC_CHECKVK(vkCreateDescriptorPool(device.getDevice(), &pool_info, nullptr, &descriptor_pool));

        // create descriptor set
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &set_layout;
        GC_CHECKVK(vkAllocateDescriptorSets(device.getDevice(), &alloc_info, &descriptor_set));

        VkDescriptorImageInfo image_info{};
        image_info.sampler = my_sampler;
        image_info.imageView = my_image_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet descriptor_write{};
        descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write.dstSet = descriptor_set;
        descriptor_write.dstBinding = 0;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device.getDevice(), 1, &descriptor_write, 0, nullptr);
    }

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

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    gc::app().jobs().execute([&pipeline, &pipeline_layout, &set_layout]() {
        std::tie(pipeline, pipeline_layout) = gc::createPipeline(set_layout);
        SDL_Delay(1000);
    });

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

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);

            const glm::quat rotation = glm::angleAxis(static_cast<float>(elapsed_time), glm::normalize(glm::vec3{0.4f, 3.0f, 1.0f}));
            const glm::vec3 position{glm::sin(elapsed_time), glm::cos(elapsed_time) * 0.5f, -3.0f};
            glm::vec3 scale{1.0f, 1.0f, 1.0f};
            scale *= 0.2f;
            // apply rotation
            glm::mat4 world_transform = glm::mat4_cast(rotation);
            // apply position
            world_transform[3][0] = position.x;
            world_transform[3][1] = position.y;
            world_transform[3][2] = position.z;
            // apply scale
            world_transform = glm::scale(world_transform, scale);
            struct PushConstants {
                glm::mat4 world_transform;
                glm::mat4 projection;
            } push_constants{};
            const VkExtent2D extent = renderer.getSwapchain().getExtent();
            const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

            push_constants.world_transform = world_transform;
            push_constants.projection = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 50.0f);
            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<uint32_t>(sizeof(PushConstants)), &push_constants);

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

    vkDestroyDescriptorPool(device.getDevice(), descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(device.getDevice(), set_layout, nullptr);
    vkDestroySampler(device.getDevice(), my_sampler, nullptr);
    vkDestroyImageView(device.getDevice(), my_image_view, nullptr);
    vmaDestroyImage(renderer.getAllocator(), my_image, my_image_allocation);

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
