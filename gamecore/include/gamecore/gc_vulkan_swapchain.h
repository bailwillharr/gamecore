#pragma once

#include <vector>
#include <span>

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

    inline VkSwapchainKHR getSwapchain() const { return m_swapchain; }

    // returns false if the swapchain could not be recreated due to window being minimised
    bool recreateSwapchain();

    inline uint32_t getImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    inline const std::vector<VkImage>& getImages() const { return m_images; }
    inline VkImageView getImageView(uint32_t index) const { return m_image_views[index]; }
    inline VkExtent2D getExtent() const { return m_extent; }
    inline VkSurfaceFormatKHR getSurfaceFormat() const { return m_surface_format; }
};

} // namespace gc