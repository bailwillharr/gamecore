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
#include <span>
#include <memory>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_gpu_resources.h"

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

class RenderTexture {
    GPUImageView m_image_view;
    bool m_uploaded;

public:
    explicit RenderTexture(GPUImageView&& image_view) : m_image_view(std::move(image_view)), m_uploaded(false) {}
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture(RenderTexture&& other) noexcept : m_image_view(std::move(other.m_image_view)), m_uploaded(other.m_uploaded) {}

    RenderTexture& operator=(const RenderTexture&) = delete;
    RenderTexture& operator=(RenderTexture&&) = delete;

    bool isUploaded()
    {
        if (m_uploaded) {
            return true;
        }
        // If the backing image is no longer in use by the queue, assuming the backing image was just created, this means the image is uploaded.
        if (m_image_view.getImage()->isFree()) {
            m_uploaded = true;
            return true;
        }
        return false;
    }

    GPUImageView& getImageView() { return m_image_view; }
};

class RenderMaterial {
    std::shared_ptr<RenderTexture> m_texture{};
    VkDescriptorSet m_descriptor_set{};

public:
    RenderMaterial(VkDevice device, VkDescriptorPool descriptor_pool, VkDescriptorSetLayout descriptor_set_layout,
                   const std::shared_ptr<RenderTexture>& texture)
        : m_texture(texture)
    {
        GC_ASSERT(m_texture);
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = descriptor_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &descriptor_set_layout;
        GC_CHECKVK(vkAllocateDescriptorSets(device, &info, &m_descriptor_set));
        VkDescriptorImageInfo descriptor_image_info{};
        descriptor_image_info.imageView = m_texture->getImageView().getHandle();
        descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptor_set;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descriptor_image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    const auto& getTexture() const { return m_texture; }

    // call getTexture().isUploaded() before binding
    VkDescriptorSet getDescriptorSet() const { return m_descriptor_set; }
};

enum class RenderSyncMode { VSYNC_ON_DOUBLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED, VSYNC_OFF };

class RenderBackend {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    GPUResourceDeleteQueue m_delete_queue;

    // global descriptor pool
    VkSampler m_sampler{};
    VkDescriptorPool m_main_desciptor_pool{};
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

    uint64_t m_frame_count{};

    bool m_command_buffer_ready = false;

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

    uint64_t m_present_finished_value{};

    VkCommandPool m_transfer_cmd_pool{};

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
    RenderTexture createTexture();
    RenderMaterial createMaterial(const std::shared_ptr<RenderTexture>& texture);

    RenderBackendInfo getInfo() const
    {
        RenderBackendInfo info{};
        info.instance = m_device.getInstance();
        info.device = m_device.getHandle();
        info.physical_device = m_device.getPhysicalDevice();
        info.main_queue = m_device.getMainQueue();
        info.main_queue_family_index = m_device.getQueueFamilyIndex();
        info.main_descriptor_pool = m_main_desciptor_pool;
        info.framebuffer_format = m_swapchain.getSurfaceFormat().format;
        info.depth_stencil_format = m_depth_stencil_format;
        return info;
    }

    VkDevice getDevice() const { return m_device.getHandle(); }

    void waitIdle();

private:
    void recreateFramesInFlightResources();

    /* Call this before input polling and logic to reduce latency at the cost of stalling GPU */
    void waitForFrameReady();
};

} // namespace gc
