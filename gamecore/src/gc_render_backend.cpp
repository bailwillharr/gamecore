#include "gamecore/gc_render_backend.h"

#include <array>

#include <SDL3/SDL_vulkan.h>

#include <tracy/Tracy.hpp>

#include <backends/imgui_impl_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"

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
        GC_CHECKVK(vkCreateDescriptorPool(m_device.getHandle(), &pool_info, nullptr, &m_desciptor_pool));
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

    // m_timeline_semaphore and the frame in flight command pools will be created when renderFrame() is called for the first time.

    GC_TRACE("Initialised RenderBackend");
}

RenderBackend::~RenderBackend()
{
    GC_TRACE("Destroying RenderBackend...");

    waitIdle();

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

    vkDestroyDescriptorPool(m_device.getHandle(), m_desciptor_pool, nullptr);
}

void RenderBackend::renderFrame(bool window_resized)
{
    if (m_requested_frames_in_flight != static_cast<int>(m_fif.size())) {
        recreateFramesInFlightResources();
    }

    auto& stuff = m_fif[m_frame_count % m_fif.size()];

    { // Wait for command buffer to be available
        ZoneScopedN("Wait for semaphore to reach:");
        ZoneValue(stuff.command_buffer_available_value);
        VkSemaphoreWaitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores = &m_timeline_semaphore;
        info.pValues = &stuff.command_buffer_available_value;
        GC_CHECKVK(vkWaitSemaphores(m_device.getHandle(), &info, UINT64_MAX));
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

    const bool swapchain_recreated = m_swapchain.acquireAndPresent(m_framebuffer_image, window_resized, m_timeline_semaphore, m_timeline_value);

    m_present_finished_value = m_timeline_value;

    if (swapchain_recreated) {
        recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                             m_depth_stencil_allocation, m_depth_stencil_view);
        recreateFramebufferImage(m_device.getHandle(), m_allocator.getHandle(), m_swapchain.getSurfaceFormat().format, m_swapchain.getExtent(),
                                 m_framebuffer_image, m_framebuffer_image_allocation, m_framebuffer_image_view);
    }
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
