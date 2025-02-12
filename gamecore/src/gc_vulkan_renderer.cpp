#include "gamecore/gc_vulkan_renderer.h"

#include <cmath>

#include <array>
#include <span>

#include <SDL3/SDL_vulkan.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_app.h"
#include "gamecore/gc_window.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_asset_id.h"
#include "gamecore/gc_compile_shader.h"

namespace gc {

static void recordCommandBuffer(const VulkanDevice& device, VkImage swapchain_image, VkImageView swapchain_image_view, VkExtent2D swapchain_extent,
                                VkCommandBuffer cmd, std::span<VkCommandBuffer> rendering_cmds)
{
    ZoneScoped;

    constexpr std::array<float, 4> CLEAR_COLOR{1.0f, 1.0f, 1.0f, 1.0f};

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_info.pInheritanceInfo = nullptr;
    GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

    { // transition undefined acquired swapchain image to a renderable color attachment image
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_NONE_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; // image is next used as a color attachment in fragment shader
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
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
        color_attachment.imageView = swapchain_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = CLEAR_COLOR[0];
        color_attachment.clearValue.color.float32[1] = CLEAR_COLOR[1];
        color_attachment.clearValue.color.float32[2] = CLEAR_COLOR[2];
        color_attachment.clearValue.color.float32[3] = CLEAR_COLOR[3];
        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        rendering_info.renderArea.offset = VkOffset2D{0, 0};
        rendering_info.renderArea.extent = swapchain_extent;
        rendering_info.layerCount = 1;
        rendering_info.viewMask = 0;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        rendering_info.pDepthAttachment = nullptr;
        rendering_info.pStencilAttachment = nullptr;
        vkCmdBeginRendering(cmd, &rendering_info);
    }

    if (!rendering_cmds.empty()) {
        vkCmdExecuteCommands(cmd, static_cast<uint32_t>(rendering_cmds.size()), rendering_cmds.data());
    }

    {
        vkCmdEndRendering(cmd);
    }

    { // transition rendered color attachment image to presentable image
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; // image is needed for the stage when the final image is coloured
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
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
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GC_CHECKVK(vkCreateSemaphore(m_device.getDevice(), &sem_info, nullptr, &per_frame_data.image_acquired_semaphore));
        GC_CHECKVK(vkCreateSemaphore(m_device.getDevice(), &sem_info, nullptr, &per_frame_data.ready_to_present_semaphore));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
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

    VkSemaphoreTypeCreateInfo sem_type{};
    sem_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_type.pNext = nullptr;
    sem_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_type.initialValue = 0;
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &sem_type;
    sem_info.flags = 0;
    GC_CHECKVK(vkCreateSemaphore(m_device.getDevice(), &sem_info, nullptr, &m_timeline_semaphore));

    GC_TRACE("Initialised VulkanRenderer");
}

VulkanRenderer::~VulkanRenderer()
{
    GC_TRACE("Destroying VulkanRenderer...");

    waitIdle();

    vkDestroySemaphore(m_device.getDevice(), m_timeline_semaphore, nullptr);

    for (auto& per_frame_data : m_per_frame_in_flight) {
        vkFreeCommandBuffers(m_device.getDevice(), per_frame_data.pool, 1, &per_frame_data.cmd);
        vkDestroyCommandPool(m_device.getDevice(), per_frame_data.pool, nullptr);
        vkDestroySemaphore(m_device.getDevice(), per_frame_data.ready_to_present_semaphore, nullptr);
        vkDestroySemaphore(m_device.getDevice(), per_frame_data.image_acquired_semaphore, nullptr);
    }
}

void VulkanRenderer::waitForRenderFinished()
{
    ZoneScopedNC("Wait for render finished", tracy::Color::Crimson);
    if (m_framecount < VULKAN_FRAMES_IN_FLIGHT) {
        //return;
    }
    else {
        // wait for v-sync is done here, if the present mode actually waits for v-sync
        VkSemaphoreWaitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.pNext = nullptr;
        wait_info.flags = 0;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &m_timeline_semaphore;
        const uint64_t value = m_framecount - (VULKAN_FRAMES_IN_FLIGHT - 1);
        wait_info.pValues = &value;
        GC_CHECKVK(vkWaitSemaphores(m_device.getDevice(), &wait_info, UINT64_MAX));
    }
}

void VulkanRenderer::acquireAndPresent(std::span<VkCommandBuffer> rendering_cmds)
{
    bool recreate_swapchain = app().window().getResizedFlag();

    uint32_t frame_in_flight_index = getFrameInFlightIndex();

    uint32_t image_index{};

    { // No blocking here. takes something like 2us. Apparently it *can* block
        ZoneScopedN("Acquire swapchain image");
        if (VkResult res = vkAcquireNextImageKHR(m_device.getDevice(), m_swapchain.getSwapchain(), UINT64_MAX,
                                                 m_per_frame_in_flight[frame_in_flight_index].image_acquired_semaphore, VK_NULL_HANDLE, &image_index);
            res != VK_SUCCESS) {
            if (res == VK_SUBOPTIMAL_KHR) {
                GC_TRACE("vkAcquireNextImageKHR returned: {}", vulkanResToString(res));
                recreate_swapchain = true;
            }
            else {
                abortGame("vkAcquireNextImageKHR() error: {}", vulkanResToString(res));
            }
        }
    }

    /* record command buffer */
    {
        ZoneScopedN("Reset main command buffer");
        GC_CHECKVK(vkResetCommandPool(m_device.getDevice(), m_per_frame_in_flight[frame_in_flight_index].pool, 0));
    }
    recordCommandBuffer(m_device, m_swapchain.getImage(image_index), m_swapchain.getImageView(image_index), m_swapchain.getExtent(),
                        m_per_frame_in_flight[frame_in_flight_index].cmd, rendering_cmds);

    {
        ZoneScopedN("Queue submit");

        VkSemaphoreSubmitInfo wait_semaphore_info{};
        wait_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_semaphore_info.semaphore = m_per_frame_in_flight[frame_in_flight_index].image_acquired_semaphore;
        wait_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        std::array<VkSemaphoreSubmitInfo, 2> signal_semaphore_infos{};
        signal_semaphore_infos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[0].semaphore = m_per_frame_in_flight[frame_in_flight_index].ready_to_present_semaphore;
        signal_semaphore_infos[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal_semaphore_infos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[1].semaphore = m_timeline_semaphore;
        signal_semaphore_infos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // wait until command buffer is done executing
        signal_semaphore_infos[1].value = m_framecount + 1;
        VkCommandBufferSubmitInfo cmd_buf_info{};
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_buf_info.commandBuffer = m_per_frame_in_flight[frame_in_flight_index].cmd;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_semaphore_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_buf_info;
        submit_info.signalSemaphoreInfoCount = static_cast<uint32_t>(signal_semaphore_infos.size());
        submit_info.pSignalSemaphoreInfos = signal_semaphore_infos.data();
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue().queue, 1, &submit_info, VK_NULL_HANDLE));
    }

    {
        ZoneScopedN("Queue present");

        const VkSwapchainKHR swapchain = m_swapchain.getSwapchain();
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &m_per_frame_in_flight[frame_in_flight_index].ready_to_present_semaphore;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        present_info.pResults = nullptr;
        if (VkResult res = vkQueuePresentKHR(m_device.getMainQueue().queue, &present_info); res != VK_SUCCESS) {
            if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
                GC_TRACE("vkQueuePresentKHR returned: {}", vulkanResToString(res));
                recreate_swapchain = true;
            }
            else {
                abortGame("vkQueuePresentKHR() error: {}", vulkanResToString(res));
            }
        }
    }

    if (recreate_swapchain) {
        GC_CHECKVK(vkDeviceWaitIdle(m_device.getDevice()));
        m_swapchain.recreateSwapchain();
    }

    ++m_framecount;
}

uint64_t VulkanRenderer::getFramecount() const { return m_framecount; }

void VulkanRenderer::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getDevice()));
}

} // namespace gc
