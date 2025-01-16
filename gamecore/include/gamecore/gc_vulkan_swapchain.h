#pragma once

#include <vector>

#include <SDL3/SDL_video.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"

namespace gc {

class VulkanSwapchain {
    const VulkanDevice& m_device;
    SDL_Window* const m_window_handle;

    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    VkSurfaceFormatKHR m_surface_format{};
    VkFormat m_depth_stencil_format{};
    VkPresentModeKHR m_present_mode{};
    VkExtent2D m_extent{};

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images{};
    std::vector<VkImageView> m_image_views{};

public:
    VulkanSwapchain(const VulkanDevice& device, SDL_Window* window);
    VulkanSwapchain(const VulkanSwapchain&) = delete;

    ~VulkanSwapchain();

    VulkanSwapchain operator=(const VulkanSwapchain&) = delete;

    inline const VkSwapchainKHR& getSwapchain() { return m_swapchain; }

    void recreateSwapchain();

    inline uint32_t getImageCount() { return static_cast<uint32_t>(m_images.size()); }
    inline VkImage getImage(uint32_t index) { return m_images[index]; }
    inline VkImageView getImageView(uint32_t index) { return m_image_views[index]; }
    inline VkExtent2D getExtent() { return m_extent; }
};

} // namespace gc