#include "gamecore/gc_render_backend.h"

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
                                 VmaAllocation& depth_stencil_allocation, VkImageView& depth_stencil_view)
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

static void recreateFramebufferImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& image, VmaAllocation& allocation,
                                     VkImageView& view)
{
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, image, allocation);
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
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;     // ignored
    image_info.pQueueFamilyIndices = nullptr; // ingored
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_create_info.priority = 1.0f;
    GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &image, &allocation, nullptr));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &view));
}

RenderBackend::RenderBackend(SDL_Window* window_handle) : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle)
{
    {
        std::array<VkDescriptorPoolSize, 1> pool_sizes = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes) {
            pool_info.maxSets += pool_size.descriptorCount;
        }
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        GC_CHECKVK(vkCreateDescriptorPool(m_device.getHandle(), &pool_info, nullptr, &m_desciptor_pool));
    }

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

    { /* This stuff must be done every time the swapchain is recreated */
        recreateDepthStencil(m_device.getHandle(), m_allocator.getHandle(), m_depth_stencil_format, m_swapchain.getExtent(), m_depth_stencil,
                                   m_depth_stencil_allocation, m_depth_stencil_view);
        recreateFramebufferImage(m_device.getHandle(), m_allocator.getHandle(), m_swapchain.getSurfaceFormat().format, m_swapchain.getExtent(), m_framebuffer_image,
                                 m_framebuffer_image_allocation, m_framebuffer_image_view);
    }

    GC_TRACE("Initialised RenderBackend");
}

RenderBackend::~RenderBackend()
{
    GC_TRACE("Destroying RenderBackend...");

    waitIdle();

    vkDestroyImageView(m_device.getHandle(), m_framebuffer_image_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_framebuffer_image, m_framebuffer_image_allocation);

    vkDestroyImageView(m_device.getHandle(), m_depth_stencil_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil, m_depth_stencil_allocation);

    vkDestroyDescriptorPool(m_device.getHandle(), m_desciptor_pool, nullptr);
}

void RenderBackend::renderFrame()
{
    if (m_requested_frames_in_flight != static_cast<int>(m_fif.size())) {

    }
}

void RenderBackend::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
}

void RenderBackend::recreateFramesInFlightResources()
{
    // wait for any work on the queue used for rendering and presentation is finished.
    GC_CHECKVK(vkQueueWaitIdle(m_device.getMainQueue()));

    if (m_timeline_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device.getHandle(), m_timeline_semaphore, nullptr);
    }

    {
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type_info;
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &info, nullptr, &timeline_semaphore));
    }

    m_timeline_value = 0;
    m_present_finished_value = 0;

    for (const auto& stuff : fif) {
        vkDestroyCommandPool(renderer.getDevice().getHandle(), stuff.pool, nullptr);
    }

    fif.resize(frames_in_flight);

    /* Create 1 command buffer per frame in flight */
    for (auto& stuff : fif) {
        {
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            info.queueFamilyIndex = renderer.getDevice().getMainQueueFamilyIndex();
            GC_CHECKVK(vkCreateCommandPool(renderer.getDevice().getHandle(), &info, nullptr, &stuff.pool));
        }
        {
            VkCommandBufferAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandBufferCount = 1;
            info.commandPool = stuff.pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            GC_CHECKVK(vkAllocateCommandBuffers(renderer.getDevice().getHandle(), &info, &stuff.cmd));
        }
        stuff.command_buffer_available_value = 0;
    }
}

} // namespace gc
