#pragma once
#pragma once

#include <array>
#include <vector>
#include <tuple>
#include <span>

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"

namespace gc {

inline constexpr int VULKAN_FRAMES_IN_FLIGHT = 1;

struct VulkanPerFrameInFlight {
    //VkFence rendering_finished_fence = VK_NULL_HANDLE;       // starts signalled
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
    VkSemaphore m_timeline_semaphore = VK_NULL_HANDLE;

    std::array<VulkanPerFrameInFlight, VULKAN_FRAMES_IN_FLIGHT> m_per_frame_in_flight{};

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    void waitForRenderFinished();

    // Call to render the frame. This function should execute relatively quickly if V-sync is off.
    // Ideally, command buffers will be recorded in other threads.
    // This thread will submit command buffers and present the result of the last frame's queueSubmit().
    void acquireAndPresent(std::span<VkCommandBuffer> rendering_cmds);

    uint64_t getFramecount() const;
    uint32_t getFrameInFlightIndex() const { return m_framecount % VULKAN_FRAMES_IN_FLIGHT; }

    const VulkanDevice& getDevice() const { return m_device; }
    const VulkanSwapchain& getSwapchain() const { return m_swapchain; }

    void waitIdle();

};

} // namespace gc
