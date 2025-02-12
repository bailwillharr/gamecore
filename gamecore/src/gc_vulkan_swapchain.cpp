#include "gamecore/gc_vulkan_swapchain.h"

#include <vector>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_video.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"

namespace gc {

/* Present modes: */
/* FIFO: Does not use exclusive fullscreen on Windows (composited). Highest latency as rendering is locked to monitor refresh rate. No tearing. Slowdowns will half the FPS. */
/* FIFO_RELAXED: Does not use exclusive fullscreen on Windows (composited). Allows tearing if frames are submitted late to allow FPS to 'catch up' with monitor refresh rate. */
/* MAILBOX: Does not use exclusive fullscreen on Windows (composited). Latency may be slightly higher than IMMEDIATE. No tearing. */
/* IMMEDIATE: Will use exclusive fullscreen on Windows (not composited). Probably the lowest latency option. Has tearing. */
static constexpr VkPresentModeKHR PREFERRED_PRESENT_MODE = VK_PRESENT_MODE_FIFO_KHR;
//static constexpr VkPresentModeKHR PREFERRED_PRESENT_MODE = VK_PRESENT_MODE_IMMEDIATE_KHR;

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

    // find depth stencil format to use
    {
        VkFormatProperties depth_format_props{};
        vkGetPhysicalDeviceFormatProperties(m_device.getPhysicalDevice(), VK_FORMAT_D32_SFLOAT_S8_UINT,
                                            &depth_format_props); // 100% coverage on Windows. Just use this.
        if (depth_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depth_stencil_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
        else {
            abortGame("Failed to find suitable depth-buffer image format!");
        }
    }

    recreateSwapchain();

    GC_TRACE("Initialised VulkanSwapchain");
}

VulkanSwapchain::~VulkanSwapchain()
{
    GC_TRACE("Destroying VulkanSwapchain...");
    for (VkImageView view : m_image_views) {
        vkDestroyImageView(m_device.getDevice(), view, nullptr);
    }
    vkDestroySwapchainKHR(m_device.getDevice(), m_swapchain, nullptr);
    SDL_Vulkan_DestroySurface(m_device.getInstance(), m_surface, nullptr);
}

void VulkanSwapchain::recreateSwapchain()
{

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
    if (std::find(present_modes.cbegin(), present_modes.cend(), PREFERRED_PRESENT_MODE) != present_modes.cend()) {
        GC_DEBUG("Using preferred present mode");
        m_present_mode = PREFERRED_PRESENT_MODE;
    }
    else {
        GC_DEBUG("Using FIFO present mode");
        m_present_mode = VK_PRESENT_MODE_FIFO_KHR; // FIFO is always available
    }

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
    if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR && min_image_count == 2) {
        min_image_count = 3; // triple buffering
    }

    // Extent
    if (surface_caps.surfaceCapabilities.currentExtent.width != UINT32_MAX) {
        m_extent = surface_caps.surfaceCapabilities.currentExtent;
    }
    else {
        // otherwise get extent from SDL
        int w{}, h{};
        if (!SDL_GetWindowSizeInPixels(m_window_handle, &w, &h)) {
            abortGame("SDL_GetWindowSizeInPixels() error: {}", SDL_GetError());
        }
        m_extent.width = static_cast<uint32_t>(w);
        m_extent.height = static_cast<uint32_t>(h);
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
    sc_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // it's VkImageView is used with a VkFramebuffer
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.queueFamilyIndexCount = 0;     // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.pQueueFamilyIndices = nullptr; // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.preTransform = surface_caps.surfaceCapabilities.currentTransform;
    sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_info.presentMode = m_present_mode;
    sc_info.clipped = VK_TRUE;
    sc_info.oldSwapchain = old_swapchain; // which is VK_NULL_HANDLE on first swapchain creation

    if (VkResult res = vkCreateSwapchainKHR(m_device.getDevice(), &sc_info, nullptr, &m_swapchain); res != VK_SUCCESS) {
        abortGame("vkCreateSwapchainKHR() error: {}", vulkanResToString(res));
    }

    // (destroy old swapchain)
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device.getDevice(), old_swapchain, nullptr);
    }

    // get all image handles
    uint32_t image_count{};
    if (VkResult res = vkGetSwapchainImagesKHR(m_device.getDevice(), m_swapchain, &image_count, nullptr); res != VK_SUCCESS) {
        abortGame("vkGetSwapchainImagesKHR() error: {}", vulkanResToString(res));
    }
    m_images.resize(image_count);
    if (VkResult res = vkGetSwapchainImagesKHR(m_device.getDevice(), m_swapchain, &image_count, m_images.data()); res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfacePresentModesKHR() error: {}", vulkanResToString(res));
    }

    // (destroy old image views)
    for (VkImageView image_view : m_image_views) {
        vkDestroyImageView(m_device.getDevice(), image_view, nullptr);
    }
    // create image views
    m_image_views.resize(image_count);
    for (size_t i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = nullptr;
        view_info.flags = 0;
        view_info.image = m_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = sc_info.imageFormat;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        if (VkResult res = vkCreateImageView(m_device.getDevice(), &view_info, nullptr, &m_image_views[i]); res != VK_SUCCESS) {
            abortGame("vkCreateImageView() error: {}", vulkanResToString(res));
        }
    }

    // (destroy old depth/stencil image and image view)
    // create depth/stencil image and image view

    // done
    GC_DEBUG("Recreated swapchain. new extent: ({}, {}), new image count: {}", sc_info.imageExtent.width, sc_info.imageExtent.height, image_count);
}

} // namespace gc
