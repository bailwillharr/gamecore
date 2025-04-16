#pragma once

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_assert.h"

namespace gc {

inline constexpr int VULKAN_FRAMES_IN_FLIGHT = 2;

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    VkImage m_depth_stencil;
    VkImageView m_depth_stencil_view;
    VmaAllocation m_depth_stencil_allocation;
    VkFormat m_depth_stencil_format;

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    VulkanDevice& getDevice() { return m_device; }
    VulkanSwapchain& getSwapchain() { return m_swapchain; }
    VmaAllocator getAllocator() const { return m_allocator.getHandle(); }

    VkFormat getDepthStencilFormat() const { return m_depth_stencil_format; }

    void waitIdle();
};

} // namespace gc
