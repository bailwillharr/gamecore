#include "gamecore/gc_vulkan_renderer.h"

#include <cmath>

#include <array>
#include <span>

#include <SDL3/SDL_vulkan.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_app.h"
#include "gamecore/gc_window.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_asset_id.h"
#include "gamecore/gc_compile_shader.h"

namespace gc {

static void recreateDepthStencil(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& depth_stencil,
                                 VkImageView& depth_stencil_view, VmaAllocation& depth_stencil_allocation)
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

void recreatePerSwapchainImageResources(const VulkanDevice& device, uint32_t image_count,
                                        std::vector<PerSwapchainImageResources>& resources_per_swapchain_image)
{
    if (!resources_per_swapchain_image.empty()) {
        for (const auto& resources : resources_per_swapchain_image) {
            vkDestroyCommandPool(device.getHandle(), resources.copy_image_pool, nullptr);
            vkDestroyFence(device.getHandle(), resources.command_buffer_finished, nullptr);
            vkDestroySemaphore(device.getHandle(), resources.ready_to_present, nullptr);
        }
    }
    resources_per_swapchain_image.resize(image_count);
    for (auto& resources : resources_per_swapchain_image) {
        {
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            GC_CHECKVK(vkCreateSemaphore(device.getHandle(), &info, nullptr, &resources.ready_to_present));
        }
        {
            VkFenceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            GC_CHECKVK(vkCreateFence(device.getHandle(), &info, nullptr, &resources.command_buffer_finished));
        }
        {
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            info.queueFamilyIndex = device.getMainQueueFamilyIndex();
            GC_CHECKVK(vkCreateCommandPool(device.getHandle(), &info, nullptr, &resources.copy_image_pool));
        }
        {
            VkCommandBufferAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.pNext = nullptr;
            info.commandPool = resources.copy_image_pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            GC_CHECKVK(vkAllocateCommandBuffers(device.getHandle(), &info, &resources.copy_image_cmdbuf));
        }
    }
}

VulkanRenderer::VulkanRenderer(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
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
        m_depth_stencil = VK_NULL_HANDLE;
        m_depth_stencil_view = VK_NULL_HANDLE;
        recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                             m_depth_stencil_view, m_depth_stencil_allocation);

        // Per swapchain image stuff:
        recreatePerSwapchainImageResources(m_device, m_swapchain.getImageCount(), m_resources_per_swapchain_image);
    }

    GC_TRACE("Initialised VulkanRenderer");
}

VulkanRenderer::~VulkanRenderer()
{
    GC_TRACE("Destroying VulkanRenderer...");

    waitIdle();

    for (const auto& resources : m_resources_per_swapchain_image) {
        vkDestroyCommandPool(m_device.getHandle(), resources.copy_image_pool, nullptr);
        vkDestroyFence(m_device.getHandle(), resources.command_buffer_finished, nullptr);
        vkDestroySemaphore(m_device.getHandle(), resources.ready_to_present, nullptr);
        if (resources.image_acquired != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device.getHandle(), resources.image_acquired, nullptr);
        }
    }

    vkDestroyImageView(m_device.getHandle(), m_depth_stencil_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil, m_depth_stencil_allocation);
}

void VulkanRenderer::acquireAndPresent(VkImage image_to_present)
{
    (void)image_to_present;
    // GC_ASSERT(image_to_present != VK_NULL_HANDLE);

    uint32_t image_index{};
    bool recreate_swapchain = false;

    /* Creating/destroying semaphores and fences are lightweight operations (~5us) so they're created every time an image is acquired. */
    /* Semaphores are created and then assigned to their place in the m_image_acquired_semaphores array based on the image index. */
    /* This prevents semaphores from being leaked. */
    VkSemaphore image_acquired_semaphore{};
    {
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &sem_info, nullptr, &image_acquired_semaphore));
    }

    {
        ZoneScopedN("Acquire next image");
        if (VkResult res =
                vkAcquireNextImageKHR(m_device.getHandle(), m_swapchain.getSwapchain(), UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &image_index);
            res != VK_SUCCESS) {
            if (res == VK_SUBOPTIMAL_KHR) {
                GC_TRACE("vkAcquireNextImageKHR returned: {}", vulkanResToString(res));
                recreate_swapchain = true;
            }
            else {
                abortGame("vkAcquireNextImageKHR() error: {}", vulkanResToString(res));
            }
        }
    }

    if (m_resources_per_swapchain_image[image_index].image_acquired != VK_NULL_HANDLE) {
        ZoneScopedN("Wait for swapchain image");
        GC_CHECKVK(vkWaitForFences(m_device.getHandle(), 1, &m_resources_per_swapchain_image[image_index].command_buffer_finished, VK_FALSE, UINT64_MAX));
        GC_CHECKVK(vkResetFences(m_device.getHandle(), 1, &m_resources_per_swapchain_image[image_index].command_buffer_finished));
        vkDestroySemaphore(m_device.getHandle(), m_resources_per_swapchain_image[image_index].image_acquired, nullptr);
    }
    m_resources_per_swapchain_image[image_index].image_acquired = image_acquired_semaphore;

    /* record command buffer */
    {
        ZoneScopedN("Record acquireAndPresent cmdbuf");

        GC_CHECKVK(vkResetCommandPool(m_device.getHandle(), m_resources_per_swapchain_image[image_index].copy_image_pool, 0));

        const VkCommandBuffer cmd = m_resources_per_swapchain_image[image_index].copy_image_cmdbuf;

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_info.pInheritanceInfo = nullptr;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        { // transition acquired swapchain image to TRANSFER_DST layout
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_swapchain.getImages()[image_index];
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

        /*

        VkImageCopy2 image_copy{};
        image_copy.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
        image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_copy.srcSubresource.mipLevel = 0;
        image_copy.srcSubresource.baseArrayLayer = 0;
        image_copy.srcSubresource.layerCount = 1;
        image_copy.srcOffset.x = 0;
        image_copy.srcOffset.y = 0;
        image_copy.srcOffset.z = 0;
        image_copy.dstSubresource = image_copy.srcSubresource;
        image_copy.dstOffset = image_copy.srcOffset;
        image_copy.extent.width = m_swapchain.getExtent().width;
        image_copy.extent.height = m_swapchain.getExtent().height;
        image_copy.extent.depth = 1;

        VkCopyImageInfo2 copy_info{};
        copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
        copy_info.srcImage = image_to_present;
        copy_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy_info.dstImage = m_swapchain.getImages()[image_index];
        copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_info.regionCount = 1;
        copy_info.pRegions = &image_copy;

        vkCmdCopyImage2(cmd, &copy_info);

        */

        VkClearColorValue color{};
        color.float32[0] = 1.0f;
        color.float32[1] = 0.0f;
        color.float32[2] = 0.0f;
        color.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, m_swapchain.getImages()[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

        { // transition acquired swapchain image to PRESENT_SRC layout
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_swapchain.getImages()[image_index];
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
    }

    { /* Copy the parameter image to the retrieved swapchain image. */
        ZoneScopedN("Submit acquireAndPresent cmdbuf");
        VkSemaphoreSubmitInfo wait_semaphore_info{};
        wait_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_semaphore_info.semaphore = m_resources_per_swapchain_image[image_index].image_acquired;
        wait_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal_semaphore_info{};
        signal_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_info.semaphore = m_resources_per_swapchain_image[image_index].ready_to_present;
        signal_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkCommandBufferSubmitInfo cmd_buf_info{};
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_buf_info.commandBuffer = m_resources_per_swapchain_image[image_index].copy_image_cmdbuf;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_semaphore_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_buf_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_semaphore_info;
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue(), 1, &submit_info, m_resources_per_swapchain_image[image_index].command_buffer_finished));
    }

    {
        ZoneScopedN("Queue present");

        const VkSwapchainKHR swapchain = m_swapchain.getSwapchain();
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &m_resources_per_swapchain_image[image_index].ready_to_present;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        present_info.pResults = nullptr;
        if (VkResult res = vkQueuePresentKHR(m_device.getMainQueue(), &present_info); res != VK_SUCCESS) {
            if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
                GC_TRACE("vkQueuePresentKHR returned: {}", vulkanResToString(res));
                recreate_swapchain = true;
            }
            else {
                abortGame("vkQueuePresentKHR() error: {}", vulkanResToString(res));
            }
        }
    }

    if (recreate_swapchain) {
        GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
        if (m_swapchain.recreateSwapchain()) {
            recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                                 m_depth_stencil_view, m_depth_stencil_allocation);
            recreatePerSwapchainImageResources(m_device, m_swapchain.getImageCount(), m_resources_per_swapchain_image);
        }
        else {
            gc::abortGame("I'll deal with this later... :/");
        }
    }
}

void VulkanRenderer::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
}

} // namespace gc
