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

static void recreateDepthStencil(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& depth_stencil,
                                 VkImageView& depth_stencil_view, VmaAllocation& depth_stencil_allocation)
{
    if (depth_stencil_view) {
        vkDestroyImageView(device, depth_stencil_view, nullptr);
    }
    if (depth_stencil) {
        vmaDestroyImage(allocator, depth_stencil, depth_stencil_allocation);
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = nullptr;
    image_info.flags = 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;     // ignored
    image_info.pQueueFamilyIndices = nullptr; // ingored
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_create_info.priority = 1.0f;
    GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &depth_stencil, &depth_stencil_allocation, nullptr));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = nullptr;
    view_info.flags = 0;
    view_info.image = depth_stencil;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &depth_stencil_view));
}

static void recordCommandBuffer(VkImage swapchain_image, VkImageView swapchain_image_view, VkImage depth_stencil, VkImageView depth_stencil_view,
                                VkExtent2D swapchain_extent, VkCommandBuffer cmd, std::span<VkCommandBuffer> rendering_cmds)
{
    ZoneScoped;

    // constexpr std::array<float, 4> CLEAR_COLOR{1.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::array<float, 4> CLEAR_COLOR{0.0f, 0.0f, 0.0f, 1.0f};

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_info.pInheritanceInfo = nullptr;
    GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

    { // transition undefined acquired swapchain image to a renderable color attachment image
        std::array<VkImageMemoryBarrier2, 2> barriers{};

        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; // image is next used as a color attachment in fragment shader
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = swapchain_image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;

        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; // previous read doesn't matter
        barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = depth_stencil;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        barriers[1].subresourceRange.baseMipLevel = 0;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.baseArrayLayer = 0;
        barriers[1].subresourceRange.layerCount = 1;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
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
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_stencil_view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // no need to preserve depth buffer after render pass
        depth_attachment.clearValue.depthStencil.depth = 1.0f;
        depth_attachment.clearValue.depthStencil.stencil = 0;
        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        rendering_info.renderArea.offset = VkOffset2D{0, 0};
        rendering_info.renderArea.extent = swapchain_extent;
        rendering_info.layerCount = 1;
        rendering_info.viewMask = 0;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        rendering_info.pDepthAttachment = &depth_attachment;
        rendering_info.pStencilAttachment = &depth_attachment;
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

static void recreatePerSwapchainImageResources(VulkanDevice& device, std::vector<PerSwapchainImageResources>& resources, const std::vector<VkImage>& swapchain_images, VkExtent2D extent)
{
    for (const PerSwapchainImageResources& per_swapchain_image : resources) {
        vkDestroySemaphore(device.getHandle(), per_swapchain_image.image_acquired, nullptr);
        vkDestroySemaphore(device.getHandle(), per_swapchain_image.ready_to_present, nullptr);
        vkDestroyCommandPool(device.getHandle(), per_swapchain_image.copy_image_pool, nullptr);
    }

    resources.resize(swapchain_images.size());
    for (int image_index = 0; image_index < static_cast<int>(swapchain_images.size()), ++image_index) {
        PerSwapchainImageResources& per_swapchain_image = resources[image_index];

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GC_CHECKVK(vkCreateSemaphore(device.getHandle(), &sem_info, nullptr, &per_swapchain_image.image_acquired));
        GC_CHECKVK(vkCreateSemaphore(device.getHandle(), &sem_info, nullptr, &per_swapchain_image.ready_to_present));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = 0;
        pool_info.queueFamilyIndex = device.getMainQueueFamilyIndex();
        GC_CHECKVK(vkCreateCommandPool(device.getHandle(), &pool_info, nullptr, &per_swapchain_image.copy_image_pool));

        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = per_swapchain_image.copy_image_pool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        GC_CHECKVK(vkAllocateCommandBuffers(device.getHandle(), &cmdAllocInfo, &per_swapchain_image.copy_image_cmdbuf));

        /* record command buffer */
        {
            const VkCommandBuffer cmd = per_swapchain_image.copy_image_cmdbuf;

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = 0;
            begin_info.pInheritanceInfo = nullptr;
            GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

            // transition acquired swapchain image to TRANSFER_DST layout
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = swapchain_images[image_index];
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

            VkImageCopy2 image_copy{};
            image_copy.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
            image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            image_copy.srcSubresource.mipLevel = 0;
            image_copy.srcSubresource.baseArrayLayer = 0;
            image_copy.srcSubresource.layerCount = 1;
            image_copy.srcOffset.x = 0;
            image_copy.srcOffset.y = 0;
            image_copy.srcOffset.z = 0;
            image_copy.dstSubresource = image_copy.srcSubresource;
            image_copy.dstOffset = image_copy.srcOffset;
            image_copy.extent.width = extent.width;
            image_copy.extent.height = extent.height;
            image_copy.extent.depth = 1;

            VkCopyImageInfo2 copy_info{};
            copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
            copy_info.srcImage = image_to_present;
            copy_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy_info.dstImage = m_swapchain.getImage(image_index);
            copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copy_info.regionCount = 1;

            vkCmdCopyImage2(cmd, &copy_info);

            // transition acquired swapchain image to PRESENT_SRC layout
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_swapchain.getImage(image_index);
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

            GC_CHECKVK(vkEndCommandBuffer(cmd));
        }
    }
}

VulkanRenderer::VulkanRenderer(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
    VkSemaphoreTypeCreateInfo sem_type{};
    sem_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_type.pNext = nullptr;
    sem_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_type.initialValue = 0;
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &sem_type;
    sem_info.flags = 0;
    GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &sem_info, nullptr, &m_timeline_semaphore));

    // find depth stencil format to use
    {
        VkFormatProperties depth_format_props{};
        vkGetPhysicalDeviceFormatProperties(m_device.getPhysicalDevice(), VK_FORMAT_D24_UNORM_S8_UINT,
                                            &depth_format_props); // NVIDIA Best practices recommend this
        if (depth_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depth_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
        }
        else {
            abortGame("Failed to find suitable depth-buffer image format!");
        }
    }
    m_depth_stencil = VK_NULL_HANDLE;
    m_depth_stencil_view = VK_NULL_HANDLE;
    recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil, m_depth_stencil_view,
                         m_depth_stencil_allocation);

    GC_TRACE("Initialised VulkanRenderer");
}

VulkanRenderer::~VulkanRenderer()
{
    GC_TRACE("Destroying VulkanRenderer...");

    waitIdle();

    vkDestroyImageView(m_device.getHandle(), m_depth_stencil_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil, m_depth_stencil_allocation);

    vkDestroySemaphore(m_device.getHandle(), m_timeline_semaphore, nullptr);

    for (auto& per_swapchain_image : m_swapchain_image_resources) {
        vkDestroySemaphore(m_device.getHandle(), per_swapchain_image.image_acquired, nullptr);
        vkDestroySemaphore(m_device.getHandle(), per_swapchain_image.ready_to_present, nullptr);
        vkDestroyCommandPool(m_device.getHandle(), per_swapchain_image.copy_image_pool, nullptr);
    }
}

void VulkanRenderer::waitForPresentFinished()
{
    ZoneScopedNC("Wait for render finished", tracy::Color::Crimson);
    if (m_framecount < VULKAN_FRAMES_IN_FLIGHT) {
        // return;
    }
    else {
        // wait for v-sync is done here, if the present mode actually waits for v-sync
        VkSemaphoreWaitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.pNext = nullptr;
        wait_info.flags = 0;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &m_timeline_semaphore;
        const uint64_t value = m_framecount - static_cast<uint64_t>(VULKAN_FRAMES_IN_FLIGHT - 1);
        wait_info.pValues = &value;
        GC_CHECKVK(vkWaitSemaphores(m_device.getHandle(), &wait_info, UINT64_MAX));
    }
}

void VulkanRenderer::acquireAndPresent(VkImage image_to_present)
{
    bool recreate_swapchain = app().window().getResizedFlag();

    const uint32_t frame_in_flight_index = getFrameInFlightIndex();

    uint32_t image_index{};
    if (VkResult res = vkAcquireNextImageKHR(m_device.getHandle(), m_swapchain.getSwapchain(), 0, m_image_acquired_semaphores[frame_in_flight_index],
                                             VK_NULL_HANDLE, &image_index);
        res != VK_SUCCESS) {
        if (res == VK_SUBOPTIMAL_KHR) {
            GC_TRACE("vkAcquireNextImageKHR returned: {}", vulkanResToString(res));
            recreate_swapchain = true;
        }
        else {
            abortGame("vkAcquireNextImageKHR() error: {}", vulkanResToString(res));
        }
    }

    {
        ZoneScopedN("Queue submit");

        VkSemaphoreSubmitInfo wait_semaphore_info{};
        wait_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_semaphore_info.semaphore = m_image_acquired_semaphores[frame_in_flight_index];
        wait_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        std::array<VkSemaphoreSubmitInfo, 2> signal_semaphore_infos{};
        signal_semaphore_infos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[0].semaphore = m_ready_to_present_semaphores[frame_in_flight_index];
        signal_semaphore_infos[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal_semaphore_infos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_infos[1].semaphore = m_timeline_semaphore;
        signal_semaphore_infos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // wait until command buffer is done executing
        signal_semaphore_infos[1].value = m_framecount + 1;
        VkCommandBufferSubmitInfo cmd_buf_info{};
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_buf_info.commandBuffer = m_copy_framebuffer_command_buffers[frame_in_flight_index];
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_semaphore_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_buf_info;
        submit_info.signalSemaphoreInfoCount = static_cast<uint32_t>(signal_semaphore_infos.size());
        submit_info.pSignalSemaphoreInfos = signal_semaphore_infos.data();
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue(), 1, &submit_info, VK_NULL_HANDLE));
    }

    {
        ZoneScopedN("Queue present");

        const VkSwapchainKHR swapchain = m_swapchain.getSwapchain();
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &m_ready_to_present_semaphores[frame_in_flight_index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        present_info.pResults = nullptr;
        if (VkResult res = vkQueuePresentKHR(m_device.getMainQueue(), &present_info); res != VK_SUCCESS) {
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
        GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
        if (m_swapchain.recreateSwapchain()) {
            recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                                 m_depth_stencil_view, m_depth_stencil_allocation);
        }
        else {
            m_minimised = true;
        }
    }

    ++m_framecount;
}

uint64_t VulkanRenderer::getFramecount() const { return m_framecount; }

void VulkanRenderer::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
}

} // namespace gc
