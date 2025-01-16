#include "gamecore/gc_vulkan_renderer.h"

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"

namespace gc {

static VkCommandBuffer recordCommandBuffer(const VulkanDevice& device, VkCommandPool pool)
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.pNext = nullptr;
    cmdAllocInfo.commandPool = pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    if (VkResult res = vkAllocateCommandBuffers(device.getDevice(), &cmdAllocInfo, &cmd); res != VK_SUCCESS) {
        abortGame("vkAllocateCommandBuffers() error: {}", vulkanResToString(res));
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = 0;
    begin_info.pInheritanceInfo = nullptr;
    if (VkResult res = vkBeginCommandBuffer(cmd, &begin_info); res != VK_SUCCESS) {
        abortGame("vkBeginCommandBuffer() error: {}", vulkanResToString(res));
    }

    if (VkResult res = vkEndCommandBuffer(cmd); res != VK_SUCCESS) {
        abortGame("vkEndCommandBuffer() error: {}", vulkanResToString(res));
    }

    return cmd;
}

VulkanRenderer::VulkanRenderer(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.queueFamilyIndex = m_device.getMainQueue().queue_family_index;
    if (VkResult res = vkCreateCommandPool(m_device.getDevice(), &pool_info, nullptr, &m_cmd_pool); res != VK_SUCCESS) {
        abortGame("vkCreateCommandPool() error: {}", vulkanResToString(res));
    }

    GC_CHECKVK(vkCreateBuffer(VK_NULL_HANDLE, nullptr, nullptr, nullptr));

    GC_TRACE("Initialised VulkanRenderer");
}

VulkanRenderer::~VulkanRenderer()
{
    GC_TRACE("Destroying VulkanRenderer...");

    vkDestroyCommandPool(m_device.getDevice(), m_cmd_pool, nullptr);
}

void VulkanRenderer::acquireAndPresent()
{
    uint32_t image_index{};
    // signals image_available_fence once an image is acquired
    if (VkResult res = vkAcquireNextImageKHR(m_device.getDevice(), m_swapchain.getSwapchain(), UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &image_index);
        res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        abortGame("vkAcquireNextImageKHR() error: {}", vulkanResToString(res));
    }

    if (VkResult res = vkWaitForFences(m_device.getDevice(), 1, nullptr, VK_TRUE, UINT64_MAX); res != VK_SUCCESS) {
        abortGame("vkWaitForFences() error: {}", vulkanResToString(res));
    }
    if (VkResult res = vkResetFences(m_device.getDevice(), 1, nullptr); res != VK_SUCCESS) {
        abortGame("vkResetFences() error: {}", vulkanResToString(res));
    }

    //

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 0;
    present_info.pWaitSemaphores = nullptr;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &m_swapchain.getSwapchain();
    present_info.pImageIndices = &image_index;
    present_info.pResults = nullptr;
    if (VkResult res = vkQueuePresentKHR(m_device.getMainQueue().queue, &present_info); res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        abortGame("vkQueuePresentKHR() error: {}", vulkanResToString(res));
    }
}

} // namespace gc
