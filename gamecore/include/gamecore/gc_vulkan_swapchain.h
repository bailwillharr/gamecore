#pragma once

#include <vector>
#include <span>

#include <SDL3/SDL_video.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"

namespace gc {

struct PerSwapchainImageResources {
    VkSemaphore image_acquired{};      // Recreated every time due to not knowing the image index when vkAcquireNextImageKHR is called
    VkSemaphore ready_to_present{};    // only recreated when swapchain is
    VkFence command_buffer_finished{}; // signalled at same time as ready_to_present, used to stop validation layers complaining that image_acquired is still in
                                       // use when we try to destroy it.
    VkCommandPool copy_image_pool{};   // only recreated when swapchain is
    VkCommandBuffer copy_image_cmdbuf{}; // only recreated when swapchain is
};

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

    std::vector<PerSwapchainImageResources> m_resources_per_swapchain_image{};

public:
    VulkanSwapchain(const VulkanDevice& device, SDL_Window* window);
    VulkanSwapchain(const VulkanSwapchain&) = delete;

    ~VulkanSwapchain();

    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    inline VkExtent2D getExtent() const { return m_extent; }
    inline VkSurfaceFormatKHR getSurfaceFormat() const { return m_surface_format; }

    // Call to present given image to the window.
	// Returns true if the swapchain is recreated (typically means window is resized)
	// The function will wait until timeline_semaphore reaches 'value' before copying image_to_present.
	// When the copy is complete, timeline_semaphore will be set to 'value' + 1.
	// This is the case even when the swapchain is recreated or cannot be recreated (typically because the window is minimised)
    bool acquireAndPresent(VkImage image_to_present, bool window_resized, VkSemaphore timeline_semaphore, uint64_t value);

private:
    // returns false if the swapchain could not be recreated due to window being minimised
    bool recreateSwapchain();
    
};

} // namespace gc
