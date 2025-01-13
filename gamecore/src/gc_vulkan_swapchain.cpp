#include "gamecore/gc_vulkan_swapchain.h"

#include <vector>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_video.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"

namespace gc {

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

    recreateSwapchain();
}

VulkanSwapchain::~VulkanSwapchain()
{
    vkDestroySwapchainKHR(m_device.getDevice(), m_swapchain, nullptr);
    SDL_Vulkan_DestroySurface(m_device.getInstance(), m_surface, nullptr);
}

void VulkanSwapchain::recreateSwapchain()
{

    // get capabilities. These can change, for example, if the window is made fullscreen or moved to another monitor.
    VkSurfaceCapabilitiesKHR surface_caps{};
    if (VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device.getPhysicalDevice(), m_surface, &surface_caps); res != VK_SUCCESS) {
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
            m_surface_format = surface_format;
            if (surface_format.format == VK_FORMAT_B8G8R8A8_SRGB) {
                break;
            }
        }
    }
    if (m_surface_format.colorSpace != VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
        GC_WARN("Swapchain surface is not using VK_COLORSPACE_SRGB_NONLINEAR_KHR");
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
    if (std::find(present_modes.cbegin(), present_modes.cend(), VK_PRESENT_MODE_MAILBOX_KHR) != present_modes.cend()) {
        m_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    else {
        m_present_mode = VK_PRESENT_MODE_FIFO_KHR; // FIFO is always available
    }

    // Get min image count
    uint32_t min_image_count = surface_caps.minImageCount + 1;
    if (surface_caps.maxImageCount > 0 && min_image_count > surface_caps.maxImageCount) {
        min_image_count = surface_caps.maxImageCount;
    }

    // Extent
    if (surface_caps.currentExtent.width != UINT32_MAX) {
        m_extent = surface_caps.currentExtent;
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

    // Finally create swapchain
    const VkSwapchainKHR old_swapchain = m_swapchain;
    VkSwapchainCreateInfoKHR sc_info{};
    sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.pNext = nullptr;
    sc_info.flags = 0;
    sc_info.surface = m_surface;
    sc_info.minImageCount = min_image_count;
    sc_info.imageFormat = m_surface_format.format;
    sc_info.imageColorSpace = m_surface_format.colorSpace;
    sc_info.imageExtent = m_extent;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // it's VkImageView is used with a VkFramebuffer
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.queueFamilyIndexCount = 0;                        // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.pQueueFamilyIndices = nullptr;                    // ignored with VK_SHARING_MODE_EXCLUSIVE
    sc_info.preTransform = surface_caps.currentTransform;
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
    if (VkResult res = vkGetSwapchainImagesKHR(m_device.getDevice(), m_swapchain, &image_count, m_images.data());
        res != VK_SUCCESS) {
        abortGame("vkGetPhysicalDeviceSurfacePresentModesKHR() error: {}", vulkanResToString(res));
    }

    // (destroy old image views)
    // create image views

    // (destroy old depth/stencil image and image view)
    // create depth/stencil image and image view

    // done
}

} // namespace gc