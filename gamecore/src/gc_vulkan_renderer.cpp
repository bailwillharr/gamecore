#include "gamecore/gc_vulkan_renderer.h"

#include <cmath>

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"

namespace gc {

static void recordCommandBuffer(const VulkanDevice& device, VkImage swapchain_image, VkImageView image_view, VkExtent2D image_extent, VkCommandBuffer cmd,
                                uint64_t framecount)
{
    constexpr bool FUN_CLEAR_COLOR = false;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = 0;
    begin_info.pInheritanceInfo = nullptr;
    GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_NONE_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; // image is needed for the stage when the final image is coloured
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchain_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    {
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (FUN_CLEAR_COLOR) {
            color_attachment.clearValue.color.float32[0] = (sinf(static_cast<float>(framecount) * 0.010f) + 1.0f) * 0.5f;
            color_attachment.clearValue.color.float32[1] = (cosf(static_cast<float>(framecount) * 0.026f) + 1.0f) * 0.5f;
            color_attachment.clearValue.color.float32[2] = (sinf(static_cast<float>(framecount) * 0.040f) + 1.0f) * 0.5f;
            color_attachment.clearValue.color.float32[3] = 1.0f;
        }
        else {
            color_attachment.clearValue.color.float32[0] = 1.0f;
            color_attachment.clearValue.color.float32[1] = 1.0f;
            color_attachment.clearValue.color.float32[2] = 1.0f;
            color_attachment.clearValue.color.float32[3] = 1.0f;
        }
        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = VkOffset2D{0, 0};
        rendering_info.renderArea.extent = image_extent;
        rendering_info.layerCount = 1;
        rendering_info.viewMask = 0;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        rendering_info.pDepthAttachment = nullptr;
        rendering_info.pStencilAttachment = nullptr;
        vkCmdBeginRendering(cmd, &rendering_info);
    }

    {
        vkCmdEndRendering(cmd);
    }

    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; // image is needed for the stage when the final image is coloured
        barrier.dstAccessMask = VK_ACCESS_NONE;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchain_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    GC_CHECKVK(vkEndCommandBuffer(cmd));
}

VulkanRenderer::VulkanRenderer(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
    for (auto& per_frame_data : m_per_frame_in_flight) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        GC_CHECKVK(vkCreateFence(m_device.getDevice(), &fence_info, nullptr, &per_frame_data.rendering_finished_fence));

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GC_CHECKVK(vkCreateSemaphore(m_device.getDevice(), &sem_info, nullptr, &per_frame_data.image_acquired_semaphore));
        GC_CHECKVK(vkCreateSemaphore(m_device.getDevice(), &sem_info, nullptr, &per_frame_data.ready_to_present_semaphore));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = 0;
        pool_info.queueFamilyIndex = m_device.getMainQueue().queue_family_index;
        GC_CHECKVK(vkCreateCommandPool(m_device.getDevice(), &pool_info, nullptr, &per_frame_data.pool));

        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = per_frame_data.pool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        GC_CHECKVK(vkAllocateCommandBuffers(m_device.getDevice(), &cmdAllocInfo, &per_frame_data.cmd));
    }

    GC_TRACE("Initialised VulkanRenderer");
}

VulkanRenderer::~VulkanRenderer()
{
    GC_TRACE("Destroying VulkanRenderer...");

    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getDevice()));

    for (auto& per_frame_data : m_per_frame_in_flight) {
        vkFreeCommandBuffers(m_device.getDevice(), per_frame_data.pool, 1, &per_frame_data.cmd);
        vkDestroyCommandPool(m_device.getDevice(), per_frame_data.pool, nullptr);
        vkDestroySemaphore(m_device.getDevice(), per_frame_data.ready_to_present_semaphore, nullptr);
        vkDestroySemaphore(m_device.getDevice(), per_frame_data.image_acquired_semaphore, nullptr);
        vkDestroyFence(m_device.getDevice(), per_frame_data.rendering_finished_fence, nullptr);
    }
}

void VulkanRenderer::acquireAndPresent()
{
    uint32_t frame_in_flight_index = m_framecount % VULKAN_FRAMES_IN_FLIGHT;

    GC_CHECKVK(vkWaitForFences(m_device.getDevice(), 1, &m_per_frame_in_flight[frame_in_flight_index].rendering_finished_fence, VK_TRUE, UINT64_MAX));
    GC_CHECKVK(vkResetFences(m_device.getDevice(), 1, &m_per_frame_in_flight[frame_in_flight_index].rendering_finished_fence));

    uint32_t image_index{};
    if (VkResult res = vkAcquireNextImageKHR(m_device.getDevice(), m_swapchain.getSwapchain(), UINT64_MAX,
                                             m_per_frame_in_flight[frame_in_flight_index].image_acquired_semaphore, VK_NULL_HANDLE, &image_index);
        res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        abortGame("vkAcquireNextImageKHR() error: {}", vulkanResToString(res));
    }

    /* record command buffer */
    GC_CHECKVK(vkResetCommandPool(m_device.getDevice(), m_per_frame_in_flight[frame_in_flight_index].pool, 0));
    recordCommandBuffer(m_device, m_swapchain.getImage(image_index), m_swapchain.getImageView(image_index), m_swapchain.getExtent(),
                        m_per_frame_in_flight[frame_in_flight_index].cmd, m_framecount);

    VkSemaphoreSubmitInfo wait_semaphore_info{};
    wait_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_semaphore_info.semaphore = m_per_frame_in_flight[frame_in_flight_index].image_acquired_semaphore;
    wait_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR;
    VkSemaphoreSubmitInfo signal_semaphore_info{};
    signal_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_semaphore_info.semaphore = m_per_frame_in_flight[frame_in_flight_index].ready_to_present_semaphore;
    signal_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR;
    VkCommandBufferSubmitInfo cmd_buf_info{};
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_buf_info.commandBuffer = m_per_frame_in_flight[frame_in_flight_index].cmd;
    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &wait_semaphore_info;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &cmd_buf_info;
    submit_info.signalSemaphoreInfoCount = 1;
    submit_info.pSignalSemaphoreInfos = &signal_semaphore_info;
    GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue().queue, 1, &submit_info, m_per_frame_in_flight[frame_in_flight_index].rendering_finished_fence));

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &m_per_frame_in_flight[frame_in_flight_index].ready_to_present_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &m_swapchain.getSwapchain();
    present_info.pImageIndices = &image_index;
    present_info.pResults = nullptr;
    if (VkResult res = vkQueuePresentKHR(m_device.getMainQueue().queue, &present_info); res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            GC_CHECKVK(vkDeviceWaitIdle(m_device.getDevice()));
            m_swapchain.recreateSwapchain();
        }
        else {
            abortGame("vkQueuePresentKHR() error: {}", vulkanResToString(res));
        }
    }

    ++m_framecount;
}

uint64_t VulkanRenderer::getFramecount() const { return m_framecount; }

} // namespace gc
