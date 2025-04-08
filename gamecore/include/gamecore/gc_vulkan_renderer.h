#pragma once

#include <array>
#include <vector>
#include <tuple>
#include <span>
#include <bitset>

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_assert.h"

namespace gc {

inline constexpr int VULKAN_FRAMES_IN_FLIGHT = 2;

struct PerSwapchainImageResources {
    VkSemaphore image_acquired{};      // Recreated every time due to not knowing the image index when vkAcquireNextImageKHR is called
    VkSemaphore ready_to_present{};    // only recreated when swapchain is
    VkFence command_buffer_finished{}; // signalled at same time as ready_to_present, used to stop validation layers complaining that image_acquired is still in
                                       // use when we try to destroy it.
    VkCommandPool copy_image_pool{};   // only recreated when swapchain is
    VkCommandBuffer copy_image_cmdbuf{}; // only recreated when swapchain is
};

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    std::vector<PerSwapchainImageResources> m_resources_per_swapchain_image{};

    VkImage m_depth_stencil;
    VkImageView m_depth_stencil_view;
    VmaAllocation m_depth_stencil_allocation;
    VkFormat m_depth_stencil_format;

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    // Call to present given image to the window.
    // The image may not be queued for presentation (skipped) if any of the following are true:
    //  - the window is minimised
    //  - swapchain is out-of-date and cannot be recreated for whatever reason
    // The function will block if no image is available yet
    void acquireAndPresent(VkImage image_to_present);

    const VulkanDevice& getDevice() const { return m_device; }
    const VulkanSwapchain& getSwapchain() const { return m_swapchain; }
    VmaAllocator getAllocator() const { return m_allocator.getHandle(); }

    VkFormat getDepthStencilFormat() const { return m_depth_stencil_format; }

    void waitIdle();
};

} // namespace gc
