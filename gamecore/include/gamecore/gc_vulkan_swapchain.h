#pragma once

#include <SDL3/SDL_video.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"

namespace gc {

class VulkanSwapchain {
    const VulkanDevice& m_device;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;

public:
    VulkanSwapchain(const VulkanDevice& device, SDL_Window* window);
    VulkanSwapchain(const VulkanSwapchain&) = delete;

    ~VulkanSwapchain();

    VulkanSwapchain operator=(const VulkanSwapchain&) = delete;
};

}