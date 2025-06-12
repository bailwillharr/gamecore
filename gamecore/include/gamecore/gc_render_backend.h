/*
 * This is the engine's rendering backend.
 * It operates at a relatively high level while directly calling the Vulkan API.
 * This is done instead of creating a Vulkan abstraction which would end up effectively being an OpenGL remake.
 * The kind of things this renderer should do include:
 *  - Managing render targets
 *  - Presenting to the screen
 *  - ImGui integration
 *  - Drawing UI
 *  - Drawing 3D meshes with materials/textures
 *  - Applying post-processing effects such as FXAA and bloom
 * Things the renderer should not do include:
 *  - Frustrum culling
 *  - GPU resource streaming (though this class should contain methods to upload/free GPU resources)
 *  - Anything that would involve accessing/modifying scene data (It should have no knowledge of what a 'scene' is)
 * For example, to render the 3D world, the application would give RenderBackend a list of GPU mesh handles, textures, etc to draw.
 * The RenderBackend should not be aware of things like LOD selection, and should assume all draw call data given to it is valid and all resources are resident
 * in GPU memory.
 */

#pragma once

#include <vector>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"

struct SDL_Window; // forward-dec

namespace gc {

class RenderBackend {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    // global descriptor pool
    VkDescriptorPool m_desciptor_pool{};

    VkImage m_framebuffer_image{};
    VmaAllocation m_framebuffer_image_allocation{};
    VkImageView m_framebuffer_image_view{};

    VkImage m_depth_stencil{};
    VkImageView m_depth_stencil_view{};
    VmaAllocation m_depth_stencil_allocation{};
    VkFormat m_depth_stencil_format{};

    uint64_t m_frame_count{};

    /* Important synchronisation objects */
    /* If the number of frames-in-flight changes, everything here is reset */
    struct FIFStuff {
        VkCommandPool pool;
        VkCommandBuffer cmd;
        uint64_t command_buffer_available_value;
    };
    std::vector<FIFStuff> m_fif{};
    int m_requested_frames_in_flight{};
    VkSemaphore m_timeline_semaphore{};
    uint64_t m_timeline_value{};
    uint64_t m_present_finished_value{};

public:
    RenderBackend(SDL_Window* window_handle);
    RenderBackend(const RenderBackend&) = delete;

    ~RenderBackend();

    RenderBackend operator=(const RenderBackend&) = delete;

    void renderFrame();

    VulkanDevice& getDevice() { return m_device; }
    VulkanSwapchain& getSwapchain() { return m_swapchain; }
    VmaAllocator getAllocator() const { return m_allocator.getHandle(); }

    VkDescriptorPool getDescriptorPool() const { return m_desciptor_pool; }

    VkFormat getDepthStencilFormat() const { return m_depth_stencil_format; }
    VkImage getDepthStencilImage() const { return m_depth_stencil; }
    VkImageView getDepthStencilImageView() const { return m_depth_stencil_view; }

    void waitIdle();

private:
    void recreateFramesInFlightResources();
};

} // namespace gc
