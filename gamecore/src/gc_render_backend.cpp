#include "gamecore/gc_render_backend.h"

#include <array>

#include <SDL3/SDL_vulkan.h>

#include <tracy/Tracy.hpp>

#include <backends/imgui_impl_vulkan.h>

#include <ext/matrix_clip_space.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_world_draw_data.h"

namespace gc {

static void recreateDepthStencil(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& depth_stencil,
                                 VmaAllocation& depth_stencil_allocation, VkImageView& depth_stencil_view)
{
    if (depth_stencil_view) {
        vkDestroyImageView(device, depth_stencil_view, nullptr);
    }
    if (depth_stencil) {
        vmaDestroyImage(allocator, depth_stencil, depth_stencil_allocation);
    }

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
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;     // ignored
    image_info.pQueueFamilyIndices = nullptr; // ingored
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_create_info.priority = 1.0f;
    GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &depth_stencil, &depth_stencil_allocation, nullptr));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = nullptr;
    view_info.flags = 0;
    view_info.image = depth_stencil;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &depth_stencil_view));
}

static void recreateFramebufferImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& image, VmaAllocation& allocation,
                                     VkImageView& view)
{
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, image, allocation);
    }

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
}

RenderBackend::RenderBackend(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
    // create main descriptor pool for long-lasting static resources
    {
        std::array<VkDescriptorPoolSize, 1> pool_sizes = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes) {
            pool_info.maxSets += pool_size.descriptorCount;
        }
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        GC_CHECKVK(vkCreateDescriptorPool(m_device.getHandle(), &pool_info, nullptr, &m_main_desciptor_pool));
    }

    // create a simple pipeline layout for all 3D stuff (for now)
    {
        VkPushConstantRange push_constant_range{};
        push_constant_range.offset = 0;
        push_constant_range.size = 128; // Guaranteed minimum size https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#limits-minmax
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 0;
        info.pSetLayouts = nullptr;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push_constant_range;
        GC_CHECKVK(vkCreatePipelineLayout(m_device.getHandle(), &info, nullptr, &m_pipeline_layout));
    }

    // find depth stencil format to use
    {
        VkFormatProperties depth_format_props{};
        vkGetPhysicalDeviceFormatProperties(m_device.getPhysicalDevice(), VK_FORMAT_D24_UNORM_S8_UINT,
                                            &depth_format_props); // NVIDIA Best practices recommend this
        if (depth_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depth_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
        }
        else {
            abortGame("Failed to find suitable depth-buffer image format!");
        }
    }

    { /* This stuff must be done every time the swapchain is recreated */
        recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                             m_depth_stencil_allocation, m_depth_stencil_view);
        recreateFramebufferImage(m_device.getHandle(), m_allocator.getHandle(), m_swapchain.getSurfaceFormat().format, m_swapchain.getExtent(),
                                 m_framebuffer_image, m_framebuffer_image_allocation, m_framebuffer_image_view);
    }

    m_requested_frames_in_flight = 2;

    // m_timeline_semaphore and the frame in flight command pools will be created when submitFrame() is called for the first time.

    GC_TRACE("Initialised RenderBackend");
}

RenderBackend::~RenderBackend()
{
    GC_TRACE("Destroying RenderBackend...");

    waitIdle();

    cleanupGPUResources();

    if (!m_delete_queue.empty()) {
        GC_WARN("One or more GPU resources are still in use at application shutdown!");
    }

    // destroy frame in flight resources
    if (m_timeline_semaphore) {
        vkDestroySemaphore(m_device.getHandle(), m_timeline_semaphore, nullptr);
    }
    for (const auto& stuff : m_fif) {
        vkDestroyCommandPool(m_device.getHandle(), stuff.pool, nullptr);
    }

    vkDestroyImageView(m_device.getHandle(), m_framebuffer_image_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_framebuffer_image, m_framebuffer_image_allocation);

    vkDestroyImageView(m_device.getHandle(), m_depth_stencil_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil, m_depth_stencil_allocation);

    vkDestroyPipelineLayout(m_device.getHandle(), m_pipeline_layout, nullptr);

    vkDestroyDescriptorPool(m_device.getHandle(), m_main_desciptor_pool, nullptr);
}

void RenderBackend::waitForFrameReady()
{
    ZoneScoped;

    const auto& stuff = m_fif[m_frame_count % m_fif.size()];

    ZoneValue(stuff.command_buffer_available_value);

    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &m_timeline_semaphore;
    info.pValues = &stuff.command_buffer_available_value;
    GC_CHECKVK(vkWaitSemaphores(m_device.getHandle(), &info, UINT64_MAX));
    m_command_buffer_ready = true;
}

void RenderBackend::submitFrame(bool window_resized, const WorldDrawData& world_draw_data)
{
    ZoneScoped;

    if (m_requested_frames_in_flight != static_cast<int>(m_fif.size())) {
        recreateFramesInFlightResources();
    }

    auto& stuff = m_fif[m_frame_count % m_fif.size()];

    // Wait for command buffer to be available
    if (!m_command_buffer_ready) {
        waitForFrameReady();
    }

    GC_CHECKVK(vkResetCommandPool(m_device.getHandle(), stuff.pool, 0));

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
        barrier.image = m_framebuffer_image;
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
        barrier.image = m_depth_stencil;
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
        color_attachment.imageView = m_framebuffer_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = 1.0f;
        color_attachment.clearValue.color.float32[1] = 1.0f;
        color_attachment.clearValue.color.float32[2] = 1.0f;
        color_attachment.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = m_depth_stencil_view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.offset.x = 0;
        info.renderArea.offset.y = 0;
        info.renderArea.extent = m_swapchain.getExtent();
        info.layerCount = 1;
        info.viewMask = 0;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color_attachment;
        info.pDepthAttachment = &depth_attachment;
        info.pStencilAttachment = nullptr;
        vkCmdBeginRendering(stuff.cmd, &info);
    }

    // Set viewport and scissor (dynamic states)

    const VkExtent2D swapchain_extent = m_swapchain.getExtent();

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

    // render provided draw_data here
    // for now just record into primary command buffer. Might change later.
    if (GPUPipeline* pipeline = world_draw_data.getPipeline()) {
        pipeline->useResource(m_timeline_semaphore, m_timeline_value + 1);
        vkCmdBindPipeline(stuff.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle);

        const double aspect_ratio = static_cast<double>(m_swapchain.getExtent().width) / static_cast<double>(m_swapchain.getExtent().height);
        glm::mat4 projection_matrix = glm::perspectiveLH_ZO(glm::radians(45.0), aspect_ratio, 0.1, 1000.0);
        vkCmdPushConstants(stuff.cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, &projection_matrix);

        for (const auto& matrix : world_draw_data.getCubeMatrices()) {
            vkCmdPushConstants(stuff.cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &matrix);
            vkCmdDraw(stuff.cmd, 36, 1, 0, 0);
        }
    }
    else {
        GC_ERROR("No pipeline set for world draw data");
    }

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), stuff.cmd);

    vkCmdEndRendering(stuff.cmd);

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
        barrier.image = m_framebuffer_image;
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

    /* Submit command buffer */
    {
        ZoneScopedN("Submit command buffer, signal with:");
        ZoneValue(m_timeline_value + 1);

        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = stuff.cmd;

        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = m_timeline_semaphore;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        wait_info.value = m_present_finished_value;

        m_timeline_value += 1;
        stuff.command_buffer_available_value = m_timeline_value;

        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_timeline_semaphore;
        signal_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        signal_info.value = m_timeline_value;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait_info;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal_info;
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue(), 1, &submit, VK_NULL_HANDLE));
    }

    m_command_buffer_ready = false;

    const bool swapchain_recreated = m_swapchain.acquireAndPresent(m_framebuffer_image, window_resized, m_timeline_semaphore, m_timeline_value);

    m_present_finished_value = m_timeline_value;

    if (swapchain_recreated) {
        recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                             m_depth_stencil_allocation, m_depth_stencil_view);
        recreateFramebufferImage(m_device.getHandle(), m_allocator.getHandle(), m_swapchain.getSurfaceFormat().format, m_swapchain.getExtent(),
                                 m_framebuffer_image, m_framebuffer_image_allocation, m_framebuffer_image_view);
    }
}

void RenderBackend::cleanupGPUResources()
{
    m_delete_queue.deleteUnusedResources(m_device.getHandle(), std::array{m_timeline_semaphore});
}

GPUPipeline RenderBackend::createPipeline(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv)
{
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;

    module_info.codeSize = vertex_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(vertex_spv.data());
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(m_device.getHandle(), &module_info, nullptr, &vertex_module));

    module_info.codeSize = fragment_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(fragment_spv.data());
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(m_device.getHandle(), &module_info, nullptr, &fragment_module));

    std::array<VkPipelineShaderStageCreateInfo, 2> stage_infos{};
    stage_infos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_infos[0].pNext = nullptr;
    stage_infos[0].flags = 0;
    stage_infos[0].pName = "main";
    stage_infos[0].pSpecializationInfo = nullptr;
    stage_infos[1] = stage_infos[0];
    stage_infos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage_infos[0].module = vertex_module;
    stage_infos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage_infos[1].module = fragment_module;

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.pNext = nullptr;
    vertex_input_state.flags = 0;
    vertex_input_state.vertexBindingDescriptionCount = 0;
    vertex_input_state.pVertexBindingDescriptions = nullptr;
    vertex_input_state.vertexAttributeDescriptionCount = 0;
    vertex_input_state.pVertexAttributeDescriptions = nullptr;

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.pNext = nullptr;
    input_assembly_state.flags = 0;
    input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_state.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = nullptr;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = nullptr;

    VkPipelineRasterizationStateCreateInfo rasterization_state{};
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.depthClampEnable = VK_FALSE;
    rasterization_state.rasterizerDiscardEnable = VK_FALSE; // enabling this will not run the fragment shaders at all
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.lineWidth = 1.0f;
    rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE; // it is actually CCW but shader flips things
    rasterization_state.depthBiasEnable = VK_FALSE;
    rasterization_state.depthBiasConstantFactor = 0.0f; // ignored
    rasterization_state.depthBiasClamp = 0.0f;          // ignored
    rasterization_state.depthBiasSlopeFactor = 0.0f;    // ignored

    const VkFormat color_attachment_format = m_swapchain.getSurfaceFormat().format;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = m_depth_stencil_format;
    rendering_info.stencilAttachmentFormat = m_depth_stencil_format;

    VkPipelineMultisampleStateCreateInfo multisample_state{};
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample_state.sampleShadingEnable = VK_FALSE;
    multisample_state.minSampleShading = 1.0f;          // ignored
    multisample_state.pSampleMask = nullptr;            // ignored
    multisample_state.alphaToCoverageEnable = VK_FALSE; // ignored
    multisample_state.alphaToOneEnable = VK_FALSE;      // ignored

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};
    depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;
    depth_stencil_state.stencilTestEnable = VK_FALSE;
    depth_stencil_state.front = {};
    depth_stencil_state.back = {};

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blend_state{};
    color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.logicOpEnable = VK_FALSE;
    color_blend_state.logicOp = VK_LOGIC_OP_COPY; // ignored
    color_blend_state.attachmentCount = 1;
    color_blend_state.pAttachments = &color_blend_attachment;
    color_blend_state.blendConstants[0] = 0.0f; // ignored
    color_blend_state.blendConstants[1] = 0.0f; // ignored
    color_blend_state.blendConstants[2] = 0.0f; // ignored
    color_blend_state.blendConstants[3] = 0.0f; // ignored

    std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext = nullptr;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering_info;
    info.flags = 0;
    info.stageCount = static_cast<uint32_t>(stage_infos.size());
    info.pStages = stage_infos.data();
    info.pVertexInputState = &vertex_input_state;
    info.pInputAssemblyState = &input_assembly_state;
    info.pTessellationState = nullptr;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization_state;
    info.pMultisampleState = &multisample_state;
    info.pDepthStencilState = &depth_stencil_state;
    info.pColorBlendState = &color_blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = m_pipeline_layout;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = -1;

    VkPipeline handle{};
    GC_CHECKVK(vkCreateGraphicsPipelines(m_device.getHandle(), VK_NULL_HANDLE, 1, &info, nullptr, &handle));
    
    vkDestroyShaderModule(m_device.getHandle(), fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), vertex_module, nullptr);

    return GPUPipeline(m_delete_queue, handle);
}

void RenderBackend::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
}

void RenderBackend::recreateFramesInFlightResources()
{
    // wait for any work on the queue used for rendering and presentation is finished.
    GC_CHECKVK(vkQueueWaitIdle(m_device.getMainQueue()));

    if (m_timeline_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device.getHandle(), m_timeline_semaphore, nullptr);
    }

    {
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type_info;
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &info, nullptr, &m_timeline_semaphore));
    }

    m_timeline_value = 0;
    m_present_finished_value = 0;

    for (const auto& stuff : m_fif) {
        vkDestroyCommandPool(m_device.getHandle(), stuff.pool, nullptr);
    }

    m_fif.resize(m_requested_frames_in_flight);

    /* Create 1 command buffer per frame in flight */
    for (auto& stuff : m_fif) {
        {
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            info.queueFamilyIndex = m_device.getMainQueueFamilyIndex();
            GC_CHECKVK(vkCreateCommandPool(m_device.getHandle(), &info, nullptr, &stuff.pool));
        }
        {
            VkCommandBufferAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandBufferCount = 1;
            info.commandPool = stuff.pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &info, &stuff.cmd));
        }
        stuff.command_buffer_available_value = 0;
    }
}

} // namespace gc
