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

#include <tracy/Tracy.hpp>

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <stdexcept>
#include <string>
#include <vector>

// Function to create a pipeline layout for the delay compute pipeline
static VkPipelineLayout createDelayPipelineLayout(VkDevice device)
{
    // No descriptor sets or push constants are needed for the delay compute shader
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;         // No descriptor sets
    pipelineLayoutInfo.pSetLayouts = nullptr;
    pipelineLayoutInfo.pushConstantRangeCount = 0; // No push constants
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    VkPipelineLayout pipelineLayout;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    return pipelineLayout;
}

// Function to create a graphics pipeline for the delay shader using dynamic rendering
static VkPipeline createDelayGraphicsPipeline(VkDevice device, VkPipelineLayout pipelineLayout, VkFormat colorAttachmentFormat,
                                              VkShaderModule* outVertexShaderModule, VkShaderModule* outFragmentShaderModule)
{
    // Inline GLSL vertex shader source
    const std::string vertexShaderSource = R"(
        #version 450
        layout(location = 0) out vec3 fragColor;
        void main() {
            uint vertexIndex = gl_VertexIndex;
            uint gridSize = 1000; // 1000x1000 grid ~ 1M triangles
            uint x = vertexIndex % gridSize;
            uint y = vertexIndex / gridSize;
            float posX = (float(x) / float(gridSize - 1)) * 2.0 - 1.0;
            float posY = (float(y) / float(gridSize - 1)) * 2.0 - 1.0;
            gl_Position = vec4(posX, posY, 0.0, 1.0);
            fragColor = vec3(1.0, 0.0, 0.0); // Red color
        }
    )";

    // Inline GLSL fragment shader source
    const std::string fragmentShaderSource = R"(
        #version 450
        layout(location = 0) in vec3 fragColor;
        layout(location = 0) out vec4 outColor;
        void main() {
            outColor = vec4(fragColor, 1.0);
        }
    )";

    // Initialize shaderc compiler
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    // Compile vertex shader to SPIR-V
    shaderc::SpvCompilationResult vertexResult = compiler.CompileGlslToSpv(vertexShaderSource, shaderc_vertex_shader, "delay.vert", options);
    if (vertexResult.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error("Failed to compile vertex shader: " + vertexResult.GetErrorMessage());
    }
    std::vector<uint32_t> vertexSpirv(vertexResult.cbegin(), vertexResult.cend());

    // Compile fragment shader to SPIR-V
    shaderc::SpvCompilationResult fragmentResult = compiler.CompileGlslToSpv(fragmentShaderSource, shaderc_fragment_shader, "delay.frag", options);
    if (fragmentResult.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error("Failed to compile fragment shader: " + fragmentResult.GetErrorMessage());
    }
    std::vector<uint32_t> fragmentSpirv(fragmentResult.cbegin(), fragmentResult.cend());

    // Create vertex shader module
    VkShaderModuleCreateInfo vertexModuleInfo = {};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = vertexSpirv.size() * sizeof(uint32_t);
    vertexModuleInfo.pCode = vertexSpirv.data();
    VkShaderModule vertexShaderModule;
    if (vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &vertexShaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex shader module");
    }
    *outVertexShaderModule = vertexShaderModule;

    // Create fragment shader module
    VkShaderModuleCreateInfo fragmentModuleInfo = {};
    fragmentModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragmentModuleInfo.codeSize = fragmentSpirv.size() * sizeof(uint32_t);
    fragmentModuleInfo.pCode = fragmentSpirv.data();
    VkShaderModule fragmentShaderModule;
    if (vkCreateShaderModule(device, &fragmentModuleInfo, nullptr, &fragmentShaderModule) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertexShaderModule, nullptr);
        throw std::runtime_error("Failed to create fragment shader module");
    }
    *outFragmentShaderModule = fragmentShaderModule;

    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertexShaderModule;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragmentShaderModule;
    shaderStages[1].pName = "main";

    // Vertex input (no vertex buffers)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.pVertexBindingDescriptions = nullptr;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    vertexInputInfo.pVertexAttributeDescriptions = nullptr;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (dynamic states)
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Dynamic states for viewport and scissor
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic rendering info
    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorAttachmentFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    // Pipeline creation
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.pNext = &renderingInfo;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertexShaderModule, nullptr);
        vkDestroyShaderModule(device, fragmentShaderModule, nullptr);
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    return pipeline;
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

    const VkPipelineLayout pipeline_layout = createDelayPipelineLayout(renderer.getDevice().getHandle());

    VkShaderModule vert_mod{}, frag_mod{};
    [[maybe_unused]] VkPipeline pipeline =
        createDelayGraphicsPipeline(renderer.getDevice().getHandle(), pipeline_layout, renderer.getSwapchain().getSurfaceFormat().format, &vert_mod, &frag_mod);

    VkSemaphore timeline_semaphore{};
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

    struct FIFStuff {
        VkImage image;
        VmaAllocation allocation;
        VkImageView view;
        VkCommandPool pool;
        VkCommandBuffer cmd;
        uint64_t render_finished_value;
        uint64_t present_finished_value;
    };

    std::array<FIFStuff, 2> fif{};
    for (auto& stuff : fif) {
        createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format,
                    renderer.getSwapchain().getExtent(), stuff.image, stuff.allocation, stuff.view);

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

        stuff.render_finished_value = 0;
        stuff.present_finished_value = 0;
    }

    uint64_t frame_count = 0;
    uint64_t timeline_value = 0;

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

        auto& stuff = fif[frame_count % fif.size()];

        {
            ZoneScopedN("Wait for semaphore");
            VkSemaphoreWaitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            info.semaphoreCount = 1;
            info.pSemaphores = &timeline_semaphore;
            info.pValues = &stuff.render_finished_value;
            GC_CHECKVK(vkWaitSemaphores(renderer.getDevice().getHandle(), &info, UINT64_MAX));
        }

        {
            ZoneScopedN("Reset command pool");
            // do rendering
            GC_CHECKVK(vkResetCommandPool(renderer.getDevice().getHandle(), stuff.pool, 0));
        }

        {
            ZoneScopedN("Record cmd buf");
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
                barrier.image = stuff.image;
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

            {
                VkRenderingAttachmentInfo att{};
                att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                att.imageView = stuff.view;
                att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att.resolveMode = VK_RESOLVE_MODE_NONE;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.clearValue.color.float32[0] = (sinf(static_cast<float>(frame_count) * 0.8e-2f) + 1.0f) * 0.5f;
                att.clearValue.color.float32[1] = (cosf(static_cast<float>(frame_count) * 0.2e-2f) + 1.0f) * 0.5f;
                att.clearValue.color.float32[2] = (sinf(static_cast<float>(frame_count) * 1.0e-2f) + 1.0f) * 0.5f;
                att.clearValue.color.float32[3] = 1.0f;
                VkRenderingInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                info.renderArea.offset.x = 0;
                info.renderArea.offset.y = 0;
                info.renderArea.extent = renderer.getSwapchain().getExtent();
                info.layerCount = 1;
                info.viewMask = 0;
                info.colorAttachmentCount = 1;
                info.pColorAttachments = &att;
                info.pDepthAttachment = nullptr;
                info.pStencilAttachment = nullptr;
                vkCmdBeginRendering(stuff.cmd, &info);
            }

            // Set viewport and scissor (dynamic states)

            const VkExtent2D swapchainExtent = renderer.getSwapchain().getExtent();

            VkViewport viewport = {};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapchainExtent.width);
            viewport.height = static_cast<float>(swapchainExtent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(stuff.cmd, 0, 1, &viewport);

            VkRect2D scissor = {};
            scissor.offset = {0, 0};
            scissor.extent = swapchainExtent;
            vkCmdSetScissor(stuff.cmd, 0, 1, &scissor);

            // Bind the graphics pipeline
            vkCmdBindPipeline(stuff.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            // Issue draw call for ~1M triangles (3M vertices, 3 vertices per triangle)
            for (int i = 0; i < 100; ++i) {
                vkCmdDraw(stuff.cmd, 1000000, 1, 0, 0);
            }

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
                barrier.image = stuff.image;
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

        {
            ZoneScopedN("CPU Delay");
            SDL_DelayNS(10000000);
        }

        /* Submit command buffer */
        {
            ZoneScopedN("Submit command buffer");

            VkCommandBufferSubmitInfo cmd_info{};
            cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmd_info.commandBuffer = stuff.cmd;

            VkSemaphoreSubmitInfo wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wait_info.semaphore = timeline_semaphore;
            wait_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            wait_info.value = stuff.present_finished_value;

            timeline_value += 1;
            stuff.render_finished_value = timeline_value;

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
        const bool resized = renderer.getSwapchain().acquireAndPresent(stuff.image, win.getResizedFlag(), timeline_semaphore, timeline_value);
        timeline_value += 1;

        stuff.present_finished_value = timeline_value; // Timeline semaphore will reach this value when image can be used again.

        // Handle resize
        if (resized) {
            for (auto& stuff2 : fif) {
                vkDestroyImageView(renderer.getDevice().getHandle(), stuff2.view, nullptr);
                vmaDestroyImage(renderer.getAllocator(), stuff2.image, stuff2.allocation);
                createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format,
                            renderer.getSwapchain().getExtent(), stuff2.image, stuff2.allocation, stuff2.view);
            }
        }

        ++frame_count;
        FrameMark;
    }

    renderer.waitIdle();

    for (const auto& stuff : fif) {
        vkDestroyCommandPool(renderer.getDevice().getHandle(), stuff.pool, nullptr);
        vkDestroyImageView(renderer.getDevice().getHandle(), stuff.view, nullptr);
        vmaDestroyImage(renderer.getAllocator(), stuff.image, stuff.allocation);
    }

    vkDestroySemaphore(renderer.getDevice().getHandle(), timeline_semaphore, nullptr);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
