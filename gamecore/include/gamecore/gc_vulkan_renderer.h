#pragma once

#include <array>
#include <vector>

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"

namespace gc {

inline constexpr int VULKAN_FRAMES_IN_FLIGHT = 2;

struct VulkanPerFrameInFlight {
    VkFence rendering_finished_fence = VK_NULL_HANDLE;       // starts signalled
    VkSemaphore image_acquired_semaphore = VK_NULL_HANDLE;   // starts unsignalled
    VkSemaphore ready_to_present_semaphore = VK_NULL_HANDLE; // starts unsignalled
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    uint64_t m_framecount = 0;
    std::array<VulkanPerFrameInFlight, VULKAN_FRAMES_IN_FLIGHT> m_per_frame_in_flight{};

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    // Call to render the frame.
    void acquireAndPresent();
    uint64_t getFramecount() const;
};

} // namespace gc
