#pragma once

#include <vector>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

struct PerSwapchainImageResources {
    VkSemaphore image_acquired{};      // Recreated every time due to not knowing the image index when vkAcquireNextImageKHR is called
    VkSemaphore ready_to_present{};    // only recreated when swapchain is
    VkFence command_buffer_finished{}; // signalled at same time as ready_to_present, used to stop validation layers complaining that image_acquired is still in
                                       // use when we try to destroy it.
    VkCommandPool copy_image_pool{};   // only recreated when swapchain is
    VkCommandBuffer copy_image_cmdbuf{}; // only recreated when swapchain is
};

class VulkanPresentation {
    std::vector<PerSwapchainImageResources> m_resources_per_swapchain_image{};

public:
	VulkanPresentation();
	VulkanPresentation(const VulkanPresentation&) = delete;

	VulkanPresentation& operator=(const VulkanPresentation&) = delete;

    // Call to present given image to the window.
    // The image may not be queued for presentation (skipped) if any of the following are true:
    //  - the window is minimised
    //  - swapchain is out-of-date and cannot be recreated for whatever reason
    // The function will block if no image is available yet
    void acquireAndPresent(VkImage image_to_present);
};

}
