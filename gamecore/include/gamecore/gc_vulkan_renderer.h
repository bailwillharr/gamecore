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
    // image acquired semaphore cannot be per image as image index is not known when vkAcquireNextImageKHR() is called
    VkSemaphore ready_to_present{}; // copy complete
    VkCommandPool copy_image_pool{};
    VkCommandBuffer copy_image_cmdbuf{};
};

/* A pool of binary semaphores and fences to keep track of vkAcquireNextImageKHR() */
class SemaphorePool {
    static constexpr size_t NUM_SEMAPHORES = 8;
    VkDevice m_device;
    std::array<VkSemaphore, NUM_SEMAPHORES> m_semaphores{};
    std::array<VkFence, NUM_SEMAPHORES> m_fences{};

public:
    explicit SemaphorePool(VkDevice device) : m_device(device) {}
    inline ~SemaphorePool()
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(NUM_SEMAPHORES); ++i) {
            if (m_semaphores[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, m_semaphores[i], nullptr);
            }
            if (m_fences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, m_fences[i], nullptr);
            }
        }
    }

    /* Get a semaphore and fence that are ready to use */
    /* If none are available, wait for */
    inline uint32_t retrieve(VkSemaphore& semaphore, VkFence& fence)
    {
        GC_ASSERT(semaphore == VK_NULL_HANDLE);
        GC_ASSERT(fence == VK_NULL_HANDLE);
        for (uint32_t i = 0; i < static_cast<uint32_t>(NUM_SEMAPHORES); ++i) {
            if (m_fences[i] == VK_NULL_HANDLE) {
                VkSemaphoreCreateInfo sem_info{};
                sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                GC_CHECKVK(vkCreateSemaphore(m_device, &sem_info, nullptr, &m_semaphores[i]));
                VkFenceCreateInfo fence_info{};
                fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                GC_CHECKVK(vkCreateFence(m_device, &fence_info, nullptr, &m_fences[i]));
                
                semaphore = m_semaphores[i];
                fence = m_fences[i];
                return i;
            }
            else {
                VkResult res = vkWaitForFences(m_device, 1, &m_fences[i], VK_FALSE, 0);
                if (res == VK_SUCCESS) {
                    semaphore = m_semaphores[i];
                    fence = m_fences[i];
                    return i;
                }
            }
        }
        abortGame("SemaphorePool ran out of semaphores");
    }

    inline void release(uint32_t index)
    {
        GC_ASSERT(m_semaphore_in_use_mask.test(index) == true);
        GC_ASSERT(m_semaphores[index] != VK_NULL_HANDLE);
        GC_ASSERT(m_fences[index] != VK_NULL_HANDLE);
        m_semaphore_in_use_mask.reset(index);
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
    SemaphorePool m_image_acquired_semaphores;

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
