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

inline constexpr int VULKAN_FRAMES_IN_FLIGHT = 2;

struct PerSwapchainImageResources {
    // image acquired semaphore cannot be per image as image index is not known when vkAcquireNextImageKHR() is called
    VkSemaphore ready_to_present{}; // copy complete
    VkCommandPool copy_image_pool{};
    VkCommandBuffer copy_image_cmdbuf{};
};

/* A pool of binary semaphores */
class SemaphorePool {
    static constexpr size_t NUM_SEMAPHORES = 8;
    VkDevice m_device;
    std::array<VkSemaphore, NUM_SEMAPHORES> m_semaphores{};
    uint64_t m_semaphore_in_use_mask{};

    static_assert(NUM_SEMAPHORES <= sizeof(m_semaphore_in_use_mask) * 8);

public:
    explicit SemaphorePool(VkDevice device) : m_device(device) {}
    inline ~SemaphorePool()
    {
        for (VkSemaphore sem : m_semaphores) {
            vkDestroySemaphore(m_device, sem, nullptr);
        }
    }

    /* Get a semaphore ready to use */
    inline std::pair<VkSemaphore, uint32_t> retrieveSemaphore()
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_semaphores.size()); ++i) {
            if ((m_semaphore_in_use_mask & (1LL << i)) == 0) {
                if (m_semaphores[i] == VK_NULL_HANDLE) {
                    VkSemaphoreCreateInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    GC_CHECKVK(vkCreateSemaphore(m_device, &info, nullptr, &m_semaphores[i]));
                }
                m_semaphore_in_use_mask |= (1LL << i);
                return std::make_pair(m_semaphores[i], i);
            }
        }
        abortGame("SemaphorePool ran out of semaphores");
    }
};

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    uint64_t m_framecount = 0;
    uint64_t m_timeline_semaphore_value = 0; // increment every time
    VkSemaphore m_timeline_semaphore{};

    // acquireAndPresent() should be able to queue up all the swapchain's images for presentation before blocking (assuming v-sync off)
    std::vector<PerSwapchainImageResources> m_swapchain_image_resources{};

    VkImage m_depth_stencil;
    VkImageView m_depth_stencil_view;
    VmaAllocation m_depth_stencil_allocation;
    VkFormat m_depth_stencil_format;

    bool m_minimised = false;

public:
    VulkanRenderer(SDL_Window* window_handle);
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;

    void waitForPresentFinished();

    // Call to present given image to the window.
    // The image may not be queued for presentation (skipped) if any of the following are true:
    //  - no image can be immediately acquired
    //  - the window is minimised
    //  - swapchain is out-of-date and cannot be recreated for whatever reason
    void acquireAndPresent(VkImage image_to_present);

    uint64_t getFramecount() const;
    uint32_t getFrameInFlightIndex() const { return m_framecount % VULKAN_FRAMES_IN_FLIGHT; }

    const VulkanDevice& getHandle() const { return m_device; }
    const VulkanSwapchain& getSwapchain() const { return m_swapchain; }
    VmaAllocator getAllocator() const { return m_allocator.getHandle(); }

    VkFormat getDepthStencilFormat() const { return m_depth_stencil_format; }

    void waitIdle();
};

} // namespace gc
