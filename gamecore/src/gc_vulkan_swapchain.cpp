#include "gamecore/gc_vulkan_swapchain.h"

#include <array>
#include <vector>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_video.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"

namespace gc {

static void recreatePerSwapchainImageResources(const VulkanDevice& device, uint32_t image_count,
                                               std::vector<PerSwapchainImageResources>& resources_per_swapchain_image)
{
    if (!resources_per_swapchain_image.empty()) {
        for (auto& resources : resources_per_swapchain_image) {
            vkDestroyCommandPool(device.getHandle(), resources.copy_image_pool, nullptr);
            vkDestroyFence(device.getHandle(), resources.command_buffer_finished, nullptr);
            vkDestroySemaphore(device.getHandle(), resources.ready_to_present, nullptr);
            if (resources.image_acquired != VK_NULL_HANDLE) {
                vkDestroySemaphore(device.getHandle(), resources.image_acquired, nullptr);
                resources.image_acquired = VK_NULL_HANDLE; // acquireAndPresent() checks the handle to see if fence must be waited on
            }
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

VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SDL_Window* window_handle) : m_device(device), m_window_handle(window_handle)
{
    if (!SDL_Vulkan_GetPresentationSupport(m_device.getInstance(), m_device.getPhysicalDevice(), 0)) {
        abortGame("No Vulkan presentation support on queue family 0");
    }

    if (!SDL_Vulkan_CreateSurface(m_window_handle, m_device.getInstance(), nullptr, &m_surface)) {
        abortGame("SDL_Vulkan_CreateSurface() error: {}", SDL_GetError());
    }
    VkBool32 surface_supported{};
    if (VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(m_device.getPhysicalDevice(), 0, m_surface, &surface_supported); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfaceSupportKHR() error: {}", vulkanResToString(res));
    }
    if (surface_supported == VK_FALSE) {
        abortGame("Physical device does not support presentation to surface.");
    }

    if (!recreateSwapchain()) {
        gc::abortGame("Failed to initialise swapchain!");
    }

    // Per swapchain image stuff:
    recreatePerSwapchainImageResources(m_device, static_cast<uint32_t>(m_images.size()), m_resources_per_swapchain_image);

    GC_TRACE("Initialised VulkanSwapchain");
}

VulkanSwapchain::~VulkanSwapchain()
{
    GC_TRACE("Destroying VulkanSwapchain...");

    for (const auto& resources : m_resources_per_swapchain_image) {
        vkDestroyCommandPool(m_device.getHandle(), resources.copy_image_pool, nullptr);
        vkDestroyFence(m_device.getHandle(), resources.command_buffer_finished, nullptr);
        vkDestroySemaphore(m_device.getHandle(), resources.ready_to_present, nullptr);
        if (resources.image_acquired != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device.getHandle(), resources.image_acquired, nullptr);
        }
    }

    vkDestroySwapchainKHR(m_device.getHandle(), m_swapchain, nullptr);
    SDL_Vulkan_DestroySurface(m_device.getInstance(), m_surface, nullptr);
}

bool VulkanSwapchain::acquireAndPresent(VkImage image_to_present, bool window_resized, VkSemaphore timeline_semaphore, uint64_t& value)
{
    ZoneScoped;

    GC_ASSERT(image_to_present != VK_NULL_HANDLE);

    if (m_minimised) {
        m_minimised = !recreateSwapchain(); // false means minimised
        if (m_minimised) { // is it still minimised?
            // Do not attempt to acquire and present a swapchain image if the window is minimised (it won't work).
            // False is returned here as there is no point in the application recreating images until the window is un-minimised and its new extent is determined.
            return false;
        }
    }

    uint32_t image_index{};
    bool recreate_swapchain = false;

    /* Creating/destroying semaphores are lightweight operations (~5us) */
    /* Semaphores are created and then assigned to their place in the m_image_acquired_semaphores array based on the image index. */
    /* This prevents semaphores from being leaked. */
    // TODO: It should be possible to not have to recreate semaphores all the time
    VkSemaphore image_acquired_semaphore{};
    {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &info, nullptr, &image_acquired_semaphore));
    }

    {
        ZoneScopedN("Acquire next image");
        if (VkResult res = vkAcquireNextImageKHR(m_device.getHandle(), m_swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &image_index);
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
        ZoneScopedNC("Wait for swapchain image", tracy::Color::Crimson);
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
            barrier.image = m_images[image_index];
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

        VkImageCopy2 image_copy{};
        image_copy.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
        image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_copy.srcSubresource.mipLevel = 0;
        image_copy.srcSubresource.baseArrayLayer = 0;
        image_copy.srcSubresource.layerCount = 1;
        image_copy.dstSubresource = image_copy.srcSubresource;
        image_copy.extent.width = getExtent().width;
        image_copy.extent.height = getExtent().height;
        image_copy.extent.depth = 1;

        VkCopyImageInfo2 copy_info{};
        copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
        copy_info.srcImage = image_to_present;
        copy_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy_info.dstImage = m_images[image_index];
        copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_info.regionCount = 1;
        copy_info.pRegions = &image_copy;

        vkCmdCopyImage2(cmd, &copy_info);

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
            barrier.image = m_images[image_index];
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

        // [0] waits for image acquire, [1] waits for parameter image to be ready
        std::array<VkSemaphoreSubmitInfo, 2> wait_semaphore_infos{};
        wait_semaphore_infos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_semaphore_infos[0].semaphore = m_resources_per_swapchain_image[image_index].image_acquired;
        wait_semaphore_infos[0].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        wait_semaphore_infos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_semaphore_infos[1].semaphore = timeline_semaphore;
        wait_semaphore_infos[1].value = value;
        wait_semaphore_infos[1].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

        // [0] signals for vkQueuePresentKHR(), [1] signals for caller to indicate image_to_present can be reused
        std::array<VkSemaphoreSubmitInfo, 2> signal_semaphore_infos{};
        signal_semaphore_infos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[0].semaphore = m_resources_per_swapchain_image[image_index].ready_to_present;
        signal_semaphore_infos[0].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        signal_semaphore_infos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[1].semaphore = timeline_semaphore;

        value = value + 1; // EDITING REFERENCE PARAMETER
        signal_semaphore_infos[1].value = value;

        signal_semaphore_infos[1].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

        VkCommandBufferSubmitInfo cmd_buf_info{};
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_buf_info.commandBuffer = m_resources_per_swapchain_image[image_index].copy_image_cmdbuf;

        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 2;
        submit_info.pWaitSemaphoreInfos = wait_semaphore_infos.data();
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_buf_info;
        submit_info.signalSemaphoreInfoCount = 2;
        submit_info.pSignalSemaphoreInfos = signal_semaphore_infos.data();
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue(), 1, &submit_info, m_resources_per_swapchain_image[image_index].command_buffer_finished));
    }

    {
        ZoneScopedN("Queue present");

        const VkSwapchainKHR swapchain = m_swapchain;
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

    if (window_resized) {
        recreate_swapchain = true;
    }

    if (recreate_swapchain) {
        GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
        if (recreateSwapchain()) {
            // recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
            //                      m_depth_stencil_view, m_depth_stencil_allocation);
            recreatePerSwapchainImageResources(m_device, static_cast<uint32_t>(m_images.size()), m_resources_per_swapchain_image);
        }
        else {
            m_minimised = true;
        }
    }

    return recreate_swapchain;
}

bool VulkanSwapchain::recreateSwapchain()
{

    /* No members of the swapchain class are altered nor is the swapchain recreated if the window is minimised */

    // get capabilities. These can change, for example, if the window is made fullscreen or moved to another monitor.
    // minImageCount and maxImageCount can also change depending on desired present mode.
    VkSurfacePresentModeEXT surface_present_mode{};
    surface_present_mode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT;
    surface_present_mode.presentMode = m_present_mode;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info.pNext = &surface_present_mode;
    surface_info.surface = m_surface;
    VkSurfaceCapabilities2KHR surface_caps{};
    surface_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    if (VkResult res = vkGetPhysicalDeviceSurfaceCapabilities2KHR(m_device.getPhysicalDevice(), &surface_info, &surface_caps); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfaceCapabilities2KHR() error: {}", vulkanResToString(res));
    }

    // Extent
    {
        const VkExtent2D caps_current_extent = surface_caps.surfaceCapabilities.currentExtent;
        if (caps_current_extent.width == 0 || caps_current_extent.height == 0)
            return false; // <-- EARLY RETURN HERE IF WINDOW IS MINIMISED
        else if (caps_current_extent.width == UINT32_MAX && caps_current_extent.height == UINT32_MAX) {
            // In this case, swapchain size dictates the size of the window.
            // Just get the size from SDL
            int w{}, h{};
            if (!SDL_GetWindowSizeInPixels(m_window_handle, &w, &h)) {
                abortGame("SDL_GetWindowSizeInPixels() error: {}", SDL_GetError());
            }
            m_extent.width = w;
            m_extent.height = h;
        }
        else {
            m_extent = caps_current_extent;
        }
    }

    // Get surface present modes
    uint32_t present_mode_count{};
    if (VkResult res = vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.getPhysicalDevice(), m_surface, &present_mode_count, nullptr); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfacePresentModesKHR() error: {}", vulkanResToString(res));
    }
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    if (VkResult res = vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.getPhysicalDevice(), m_surface, &present_mode_count, present_modes.data());
        res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfacePresentModesKHR() error: {}", vulkanResToString(res));
    }
    // for now, use Mailbox if available otherwise FIFO
    if (std::find(present_modes.cbegin(), present_modes.cend(), m_requested_present_mode) != present_modes.cend()) {
        m_present_mode = m_requested_present_mode;
    }
    else {
        GC_WARN("Requested present mode is unavailable");
        m_present_mode = VK_PRESENT_MODE_FIFO_KHR; // FIFO is always available
    }

    GC_DEBUG("Using present mode: {}", vulkanPresentModeToString(m_present_mode));

    // Get surface formats
    uint32_t surface_format_count{};
    if (VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.getPhysicalDevice(), m_surface, &surface_format_count, nullptr); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfaceFormatsKHR() error: {}", vulkanResToString(res));
    }
    std::vector<VkSurfaceFormatKHR> surface_formats(surface_format_count);
    if (VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.getPhysicalDevice(), m_surface, &surface_format_count, surface_formats.data());
        res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfaceFormatsKHR() error: {}", vulkanResToString(res));
    }

    m_surface_format = surface_formats[0];
    for (const VkSurfaceFormatKHR& surface_format : surface_formats) {
        if (surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            if (surface_format.format == VK_FORMAT_B8G8R8A8_SRGB || surface_format.format == VK_FORMAT_R8G8B8A8_SRGB) {
                m_surface_format = surface_format;
                break;
            }
        }
    }

    // Get min image count
    uint32_t min_image_count = surface_caps.surfaceCapabilities.minImageCount;
    if (surface_caps.surfaceCapabilities.maxImageCount > 0 && min_image_count > surface_caps.surfaceCapabilities.maxImageCount) {
        min_image_count = surface_caps.surfaceCapabilities.maxImageCount;
    }

    GC_TRACE("Min image count: {}", min_image_count);

    // Use triple buffering
    if (m_fifo_triple_buffering) {
        if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR && min_image_count == 2 && surface_caps.surfaceCapabilities.maxImageCount >= 3) {
            min_image_count = 3;
        }
    }

    // clamp extent to min and max
    if (m_extent.width > surface_caps.surfaceCapabilities.maxImageExtent.width) {
        m_extent.width = surface_caps.surfaceCapabilities.maxImageExtent.width;
    }
    else if (m_extent.width < surface_caps.surfaceCapabilities.minImageExtent.width) {
        m_extent.width = surface_caps.surfaceCapabilities.minImageExtent.width;
    }
    if (m_extent.height > surface_caps.surfaceCapabilities.maxImageExtent.height) {
        m_extent.height = surface_caps.surfaceCapabilities.maxImageExtent.height;
    }
    else if (m_extent.width < surface_caps.surfaceCapabilities.minImageExtent.width) {
        m_extent.height = surface_caps.surfaceCapabilities.minImageExtent.height;
    }

    // Finally create swapchain
    const VkSwapchainKHR old_swapchain = m_swapchain;
    VkSwapchainPresentModesCreateInfoEXT swapchain_present_modes_info{};
    swapchain_present_modes_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT;
    swapchain_present_modes_info.pNext = nullptr;
    swapchain_present_modes_info.presentModeCount = 1;
    swapchain_present_modes_info.pPresentModes = &m_present_mode;
    VkSwapchainCreateInfoKHR sc_info{};
    sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.pNext = &swapchain_present_modes_info;
    sc_info.flags = 0;
    sc_info.surface = m_surface;
    sc_info.minImageCount = min_image_count;
    sc_info.imageFormat = m_surface_format.format;
    sc_info.imageColorSpace = m_surface_format.colorSpace;
    sc_info.imageExtent = m_extent;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.queueFamilyIndexCount = 0;     // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.pQueueFamilyIndices = nullptr; // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.preTransform = surface_caps.surfaceCapabilities.currentTransform;
    sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_info.presentMode = m_present_mode;
    sc_info.clipped = VK_TRUE;
    sc_info.oldSwapchain = old_swapchain; // which is VK_NULL_HANDLE on first swapchain creation

    if (VkResult res = vkCreateSwapchainKHR(m_device.getHandle(), &sc_info, nullptr, &m_swapchain); res != VK_SUCCESS) {
        abortGame("vkCreateSwapchainKHR() error: {}", vulkanResToString(res));
    }

    // (destroy old swapchain)
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device.getHandle(), old_swapchain, nullptr);
    }

    // get all image handles
    uint32_t image_count{};
    if (VkResult res = vkGetSwapchainImagesKHR(m_device.getHandle(), m_swapchain, &image_count, nullptr); res != VK_SUCCESS) {
        abortGame("vkGetSwapchainImagesKHR() error: {}", vulkanResToString(res));
    }
    m_images.resize(image_count);
    if (VkResult res = vkGetSwapchainImagesKHR(m_device.getHandle(), m_swapchain, &image_count, m_images.data()); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfacePresentModesKHR() error: {}", vulkanResToString(res));
    }

    // (destroy old depth/stencil image and image view)
    // create depth/stencil image and image view

    // done
    GC_DEBUG("Recreated swapchain. new extent: ({}, {}), requested image count: {}, new image count: {}", sc_info.imageExtent.width, sc_info.imageExtent.height,
             min_image_count, image_count);

    return true;
}

} // namespace gc
