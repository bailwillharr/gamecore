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

#include <array>
#include <vector>
#include <span>
#include <memory>

#include <vec2.hpp>
#include <vec3.hpp>
#include <vec4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_render_material.h"
#include "gamecore/gc_render_mesh.h"

struct SDL_Window; // forward-dec

namespace gc {

class WorldDrawData; // forward-dec

// Handles and settings needed for setting up ImGui's Vulkan backend
struct RenderBackendInfo {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physical_device;
    VkQueue main_queue;
    uint32_t main_queue_family_index;
    VkDescriptorPool main_descriptor_pool;
    VkFormat framebuffer_format;
    VkFormat depth_stencil_format;
};

enum class RenderSyncMode { VSYNC_ON_DOUBLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED, VSYNC_OFF };

class RenderBackend {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    GPUResourceDeleteQueue m_delete_queue;

    // global descriptor pool
    VkSampler m_sampler{};
    VkDescriptorPool m_main_descriptor_pool{};
    VkDescriptorSetLayout m_descriptor_set_layout{};

    // pipeline layout for most 3D rendering
    VkPipelineLayout m_pipeline_layout{};

    VkImage m_framebuffer_image{};
    VmaAllocation m_framebuffer_image_allocation{};
    VkImageView m_framebuffer_image_view{};

    VkImage m_depth_stencil{};
    VkImageView m_depth_stencil_view{};
    VmaAllocation m_depth_stencil_allocation{};
    VkFormat m_depth_stencil_format{};

    VkSampleCountFlagBits m_msaa_samples{};

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

    VkSemaphore m_main_timeline_semaphore{};
    uint64_t m_main_timeline_value{};
    VkSemaphore m_transfer_timeline_semaphore{};
    uint64_t m_transfer_timeline_value{};

    uint64_t m_framebuffer_copy_finished_value{};

    VkCommandPool m_transfer_cmd_pool{};

#ifdef TRACY_ENABLE
    struct TracyVulkanContext {
        VkCommandPool pool;
        VkCommandBuffer cmd;
        TracyVkCtx ctx;
    };
    TracyVulkanContext m_tracy_vulkan_context{};
#endif

public:
    explicit RenderBackend(SDL_Window* window_handle);
    RenderBackend(const RenderBackend&) = delete;

    ~RenderBackend();

    RenderBackend operator=(const RenderBackend&) = delete;

    /* configure renderer */
    void setSyncMode(RenderSyncMode mode);

    /* Renders to framebuffer and presents framebuffer to the screen */
    void submitFrame(bool window_resized, const WorldDrawData& world_draw_data);

    /* Destroys any GPU resources that have been added to the delete queue and are not in use */
    void cleanupGPUResources();

    GPUPipeline createPipeline(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv);
    RenderTexture createTexture(std::span<const uint8_t> r8g8b8a8_pak, bool srgb);
    RenderMaterial createMaterial(const std::shared_ptr<RenderTexture>& base_color_texture,
                                  const std::shared_ptr<RenderTexture>& occlusion_roughness_metallic_texture,
                                  const std::shared_ptr<RenderTexture>& normal_texture, const std::shared_ptr<GPUPipeline>& pipeline);
    RenderMesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices);

    RenderBackendInfo getInfo() const
    {
        RenderBackendInfo info{};
        info.instance = m_device.getInstance();
        info.device = m_device.getHandle();
        info.physical_device = m_device.getPhysicalDevice();
        info.main_queue = m_device.getMainQueue();
        info.main_queue_family_index = m_device.getQueueFamilyIndex();
        info.main_descriptor_pool = m_main_descriptor_pool;
        info.framebuffer_format = m_swapchain.getSurfaceFormat().format;
        info.depth_stencil_format = m_depth_stencil_format;
        return info;
    }

    VkDevice getDevice() const { return m_device.getHandle(); }

    void waitIdle(); // waits for all Vulkan queues to finish

private:
    void recreateFramesInFlightResources();

    /* Call this before input polling and logic to reduce latency at the cost of stalling GPU */
    void waitForFrameReady();
};

} // namespace gc
