#pragma once

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"

namespace gc {

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
    VkSemaphore image_acquired_semaphore = VK_NULL_HANDLE;

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    // Call to render the frame.
    void acquireAndPresent();
};

} // namespace gc
