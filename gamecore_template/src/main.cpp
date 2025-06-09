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
#include <gamecore/gc_vulkan_allocator.h>
#include <gamecore/gc_vulkan_swapchain.h>
#include <gamecore/gc_stopwatch.h>

#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>
#include <glm.hpp>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <array>
#include <string>
#include <vector>

static glm::vec3 rayPlaneIntersect(glm::vec3 plane_normal, float plane_distance_from_origin, glm::vec3 ray_origin, glm::vec3 ray_direction)
{
    const float t = -(glm::dot(ray_origin, plane_normal) + plane_distance_from_origin) / glm::dot(ray_direction, plane_normal);
    return ray_origin + ray_direction * t;
}

static std::pair<VkImage, VmaAllocation> createImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& image,
                                                     VmaAllocation& allocation, VkImageView& view)
{
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = nullptr;
    image_info.flags = 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;     // ignored
    image_info.pQueueFamilyIndices = nullptr; // ingored
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_create_info.priority = 1.0f;
    GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &image, &allocation, nullptr));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &view));

    return std::make_pair(image, allocation);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    VkDescriptorSetLayout descriptor_set_layout{};
    {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 0;
        GC_CHECKVK(vkCreateDescriptorSetLayout(renderer.getDevice().getHandle(), &info, nullptr, &descriptor_set_layout));
    }

    VkImage image;
    VmaAllocation allocation;
    VkImageView view;

    createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format,
                renderer.getSwapchain().getExtent(), image, allocation, view);

    struct FIFStuff {
        VkCommandPool pool;
        VkCommandBuffer cmd;
        uint64_t command_buffer_available_value;
    };
    std::vector<FIFStuff> fif{};
    fif.clear(); // will be set at start of main loop

    int frames_in_flight = 2;

    ImGuiContext* imgui_context = ImGui::CreateContext();

    // imgui config
    {
        ImGuiIO& io = ImGui::GetIO();
        const char* user_dir = SDL_GetPrefPath("bailwillharr", "gamecore_template");
        if (user_dir) {
            static auto ini_path = (std::filesystem::path(user_dir) / "imgui.ini").string();
            io.IniFilename = ini_path.c_str();
        }
        else {
            GC_ERROR("SDL_GetPrefPath() error: {}", SDL_GetError());
            io.IniFilename = nullptr;
        }
    }

    ImGui_ImplSDL3_InitForVulkan(win.getHandle());

    /* Load ImGui Functions*/
    {
        auto loader_func = [](const char* function_name, void* user_data) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr(*reinterpret_cast<VkInstance*>(user_data), function_name);
        };
        VkInstance instance = renderer.getDevice().getInstance();
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, loader_func, &instance)) {
            gc::abortGame("ImGui_ImplVulkan_LoadFunctions() error");
        }
    }

    /* Init ImGui Vulkan Backend */
    {
        VkFormat color_attachment_format = renderer.getSwapchain().getSurfaceFormat().format;
        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.pNext = nullptr;
        rendering_info.viewMask = 0;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &color_attachment_format;
        rendering_info.depthAttachmentFormat = renderer.getDepthStencilFormat();
        rendering_info.stencilAttachmentFormat = rendering_info.depthAttachmentFormat;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = renderer.getDevice().getInstance();
        info.PhysicalDevice = renderer.getDevice().getPhysicalDevice();
        info.Device = renderer.getDevice().getHandle();
        info.QueueFamily = renderer.getDevice().getMainQueueFamilyIndex();
        info.Queue = renderer.getDevice().getMainQueue();
        info.DescriptorPool = renderer.getDescriptorPool();
        info.RenderPass = VK_NULL_HANDLE;

        // ImGui Vulkan backend doesn't allow image count of one, even though one FIF is possible.
        // Setting it to 2 even with one FIF seems to work :/
        info.MinImageCount = (frames_in_flight < 2) ? 2 : static_cast<uint32_t>(frames_in_flight);
        info.ImageCount = info.MinImageCount;

        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo = rendering_info;
        if (!ImGui_ImplVulkan_Init(&info)) {
            gc::abortGame("ImGui_ImplVulkan_Init() error");
        }
    }

    /* Create pipeline to draw the cube */
    auto [pipeline, pipeline_layout] = gc::createPipeline(descriptor_set_layout);

    uint64_t frame_count = 0;

    VkSemaphore timeline_semaphore{};
    uint64_t timeline_value = 0;
    uint64_t present_finished_value = 0;

    bool wait_after_update_switch = true;
    bool wait_after_update = true;
    bool imgui_enable = true;
    int update_delay = 0;
    int recording_delay = 0;
    int pacing_delay = 0;

    while (!win.shouldQuit()) {

        if (frames_in_flight != static_cast<int>(fif.size())) {

            renderer.waitIdle();

            if (timeline_semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(renderer.getDevice().getHandle(), timeline_semaphore, nullptr);
            }

            {
                VkSemaphoreTypeCreateInfo type_info{};
                type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
                type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                type_info.initialValue = 0;
                VkSemaphoreCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                info.pNext = &type_info;
                GC_CHECKVK(vkCreateSemaphore(renderer.getDevice().getHandle(), &info, nullptr, &timeline_semaphore));
            }

            timeline_value = 0;
            present_finished_value = 0;

            for (const auto& stuff : fif) {
                vkDestroyCommandPool(renderer.getDevice().getHandle(), stuff.pool, nullptr);
            }

            fif.resize(frames_in_flight);

            /* Create 1 command buffer per frame in flight */
            for (auto& stuff : fif) {
                {
                    VkCommandPoolCreateInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                    info.queueFamilyIndex = renderer.getDevice().getMainQueueFamilyIndex();
                    GC_CHECKVK(vkCreateCommandPool(renderer.getDevice().getHandle(), &info, nullptr, &stuff.pool));
                }
                {
                    VkCommandBufferAllocateInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    info.commandBufferCount = 1;
                    info.commandPool = stuff.pool;
                    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    GC_CHECKVK(vkAllocateCommandBuffers(renderer.getDevice().getHandle(), &info, &stuff.cmd));
                }
                stuff.command_buffer_available_value = 0;
            }

            // ImGui_ImplVulkan_SetMinImageCount((frames_in_flight < 2) ? 2 : static_cast<uint32_t>(frames_in_flight));
        }

        wait_after_update = wait_after_update_switch;

        auto& stuff = fif[frame_count % fif.size()];

        if (!wait_after_update) {
            ZoneScopedN("Wait for semaphore to reach:");
            ZoneValue(stuff.command_buffer_available_value);
            VkSemaphoreWaitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            info.semaphoreCount = 1;
            info.pSemaphores = &timeline_semaphore;
            info.pValues = &stuff.command_buffer_available_value;
            GC_CHECKVK(vkWaitSemaphores(renderer.getDevice().getHandle(), &info, UINT64_MAX));
        }

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

        glm::vec3 clearColor{1.0f, 1.0f, 1.0f};
        if (win.getButtonDown(gc::MouseButton::LEFT)) {
            clearColor = glm::vec3{0.0f, 0.0f, 0.0f};
        }

        bool change_present_mode = false;

        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        if (imgui_enable) {
            ZoneScopedN("ImGui stuff");

            ImGui::ShowDemoWindow();

            ImGui::Begin("Settings");

            ImGui::Text("Present mode: %s", gc::vulkanPresentModeToString(renderer.getSwapchain().getCurrentPresentMode()).c_str());
            ImGui::Text("Image count: %d", renderer.getSwapchain().getImageCount());

            std::array<const char*, 5> present_modes{"Immediate", "Mailbox", "FIFO (DB)", "FIFO (TB)", "FIFO Relaxed"};

            int present_mode_choice = 0;
            switch (renderer.getSwapchain().getCurrentPresentMode()) {
                case VK_PRESENT_MODE_IMMEDIATE_KHR:
                    present_mode_choice = 0;
                    break;
                case VK_PRESENT_MODE_MAILBOX_KHR:
                    present_mode_choice = 1;
                    break;
                case VK_PRESENT_MODE_FIFO_KHR:
                    if (renderer.getSwapchain().getImageCount() == 2) {
                        present_mode_choice = 2;
                    }
                    else {
                        present_mode_choice = 3;
                    }
                    break;
                case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
                    present_mode_choice = 4;
                    break;
                default:
                    break;
            }

            if (ImGui::Combo("Requested present mode", &present_mode_choice, present_modes.data(), static_cast<int>(present_modes.size()))) {
                change_present_mode = true;
                switch (present_mode_choice) {
                    case 0:
                        renderer.getSwapchain().setRequestedPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR);
                        break;
                    case 1:
                        renderer.getSwapchain().setRequestedPresentMode(VK_PRESENT_MODE_MAILBOX_KHR);
                        break;
                    case 2:
                        renderer.getSwapchain().setRequestedPresentMode(VK_PRESENT_MODE_FIFO_KHR, false);
                        break;
                    case 3:
                        renderer.getSwapchain().setRequestedPresentMode(VK_PRESENT_MODE_FIFO_KHR, true);
                        break;
                    case 4:
                        renderer.getSwapchain().setRequestedPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR);
                        break;
                    default:
                        break;
                }
            }

            ImGui::Checkbox("Wait after state update", &wait_after_update_switch);

            ImGui::SliderInt("FIF", &frames_in_flight, 1, 50);

            ImGui::SliderInt("CPU Frame Pacing Delay (ms)", &pacing_delay, 0, 100);
            ImGui::SliderInt("CPU Game Update Delay (ms)", &update_delay, 0, 100);
            ImGui::SliderInt("CPU Delay During Cmd Buffer Rec. (ms)", &recording_delay, 0, 100);

            ImGui::Text("Frames in flight: %d", static_cast<int>(fif.size()));

            if (ImGui::Button("Abort game")) {
                gc::abortGame("rip");
            }

            ImGui::Checkbox("Enable imgui", &imgui_enable);

            ImGui::End();
        }

        ImGui::Render();

        SDL_DelayPrecise(static_cast<uint64_t>(update_delay) * 1000000);

        if (wait_after_update) {
            ZoneScopedN("Wait for semaphore to reach:");
            ZoneValue(stuff.command_buffer_available_value);
            VkSemaphoreWaitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            info.semaphoreCount = 1;
            info.pSemaphores = &timeline_semaphore;
            info.pValues = &stuff.command_buffer_available_value;
            GC_CHECKVK(vkWaitSemaphores(renderer.getDevice().getHandle(), &info, UINT64_MAX));
        }

        {
            ZoneScopedN("Reset command pool");
            // do rendering
            GC_CHECKVK(vkResetCommandPool(renderer.getDevice().getHandle(), stuff.pool, 0));
        }

        {
            ZoneScopedN("Record cmd buf");

            SDL_DelayPrecise(static_cast<uint64_t>(recording_delay) * 1000000);

            {
                VkCommandBufferBeginInfo info{};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                GC_CHECKVK(vkBeginCommandBuffer(stuff.cmd, &info));
            }

            /* Transition image to COLOR_ATTACHMENT_OPTIMAL layout */
            {
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.srcAccessMask = VK_ACCESS_2_NONE;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(stuff.cmd, &dep);
            }

            /* Transition depth stencil buffer to VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL layout */
            {
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = renderer.getDepthStencilImage();
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(stuff.cmd, &dep);
            }

            {
                VkRenderingAttachmentInfo color_attachment{};
                color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color_attachment.imageView = view;
                color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
                color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color_attachment.clearValue.color.float32[0] = clearColor.r;
                color_attachment.clearValue.color.float32[1] = clearColor.g;
                color_attachment.clearValue.color.float32[2] = clearColor.b;
                color_attachment.clearValue.color.float32[3] = 1.0f;
                VkRenderingAttachmentInfo depth_attachment{};
                depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attachment.imageView = renderer.getDepthStencilImageView();
                depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
                depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depth_attachment.clearValue.depthStencil.depth = 1.0f;
                VkRenderingInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                info.renderArea.offset.x = 0;
                info.renderArea.offset.y = 0;
                info.renderArea.extent = renderer.getSwapchain().getExtent();
                info.layerCount = 1;
                info.viewMask = 0;
                info.colorAttachmentCount = 1;
                info.pColorAttachments = &color_attachment;
                info.pDepthAttachment = &depth_attachment;
                info.pStencilAttachment = nullptr;
                vkCmdBeginRendering(stuff.cmd, &info);
            }

            // Set viewport and scissor (dynamic states)

            const VkExtent2D swapchain_extent = renderer.getSwapchain().getExtent();

            VkViewport viewport = {};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapchain_extent.width);
            viewport.height = static_cast<float>(swapchain_extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(stuff.cmd, 0, 1, &viewport);

            VkRect2D scissor = {};
            scissor.offset = {0, 0};
            scissor.extent = swapchain_extent;
            vkCmdSetScissor(stuff.cmd, 0, 1, &scissor);

            vkCmdBindPipeline(stuff.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            double elapsed_time = static_cast<double>(frame_count) * 1e-3;

            const float aspect = static_cast<float>(swapchain_extent.width) / static_cast<float>(swapchain_extent.height);
            glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 50.0f);

            auto mouse_pos = win.getMousePositionNorm(); // from -1.0 to 1.0, matches clip space coordinates
            const glm::vec4 ray_clip{mouse_pos[0], mouse_pos[1], -1.0, 1.0};
            const glm::vec4 ray_eye = glm::normalize(glm::inverse(projection) * ray_clip);
            const glm::vec3 position = rayPlaneIntersect(glm::vec3{0.0f, 0.0f, 1.0f}, 8.0f, glm::vec3{0.0f, 0.0f, 0.0f}, ray_eye);

            // make it rotate
            const glm::quat rotation = glm::angleAxis(static_cast<float>(elapsed_time), glm::normalize(glm::vec3{0.4f, 3.0f, 1.0f}));

            // cube is 2 units wide so make it smaller
            glm::vec3 scale = glm::vec3{1.0f, 1.0f, 1.0f} * 0.2f;

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

            push_constants.world_transform = world_transform;
            push_constants.projection = projection;
            vkCmdPushConstants(stuff.cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<uint32_t>(sizeof(PushConstants)), &push_constants);

            vkCmdDraw(stuff.cmd, 36, 1, 0, 0);

            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), stuff.cmd);

            {
                vkCmdEndRendering(stuff.cmd);
            }

            /* Transition image to TRANSFER_SRC layout */
            {
                VkImageMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(stuff.cmd, &dep);
            }

            GC_CHECKVK(vkEndCommandBuffer(stuff.cmd));
        }

        /* Submit command buffer */
        {
            ZoneScopedN("Submit command buffer, signal with:");
            ZoneValue(timeline_value + 1);

            VkCommandBufferSubmitInfo cmd_info{};
            cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmd_info.commandBuffer = stuff.cmd;

            VkSemaphoreSubmitInfo wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wait_info.semaphore = timeline_semaphore;
            wait_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            wait_info.value = present_finished_value;

            timeline_value += 1;
            stuff.command_buffer_available_value = timeline_value;

            VkSemaphoreSubmitInfo signal_info{};
            signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signal_info.semaphore = timeline_semaphore;
            signal_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            signal_info.value = timeline_value;

            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait_info;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmd_info;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal_info;
            GC_CHECKVK(vkQueueSubmit2(renderer.getDevice().getMainQueue(), 1, &submit, VK_NULL_HANDLE));
        }

        // Queue image for presentation
        const bool resized = renderer.getSwapchain().acquireAndPresent(image, win.getResizedFlag() || change_present_mode, timeline_semaphore, timeline_value);

        present_finished_value = timeline_value; // Timeline semaphore will reach this value when image can be used again.

        // Handle resize
        if (resized) {
            vkDestroyImageView(renderer.getDevice().getHandle(), view, nullptr);
            vmaDestroyImage(renderer.getAllocator(), image, allocation);
            createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format,
                        renderer.getSwapchain().getExtent(), image, allocation, view);
            renderer.recreateDepthStencil();
        }

        SDL_DelayPrecise(static_cast<uint64_t>(pacing_delay) * 1000000);

        ++frame_count;
        FrameMark;
    }

    renderer.waitIdle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(imgui_context);

    for (const auto& stuff : fif) {
        vkDestroyCommandPool(renderer.getDevice().getHandle(), stuff.pool, nullptr);
    }
    vkDestroyImageView(renderer.getDevice().getHandle(), view, nullptr);
    vmaDestroyImage(renderer.getAllocator(), image, allocation);

    vkDestroySemaphore(renderer.getDevice().getHandle(), timeline_semaphore, nullptr);

    vkDestroyPipeline(renderer.getDevice().getHandle(), pipeline, nullptr);
    vkDestroyPipelineLayout(renderer.getDevice().getHandle(), pipeline_layout, nullptr);

    vkDestroyDescriptorSetLayout(renderer.getDevice().getHandle(), descriptor_set_layout, nullptr);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
