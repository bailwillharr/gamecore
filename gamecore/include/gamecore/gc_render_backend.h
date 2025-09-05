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

struct Pipeline {
    VkPipeline pipeline;
    VkPipelineLayout layout;
};

template <typename T>
class GPUResourceDeleter {
public:
    struct DeletionEntry {
        T resource;
        VkQueue queue_using_resource;
        uint64_t resource_free_signal_value;
    };

private:
    std::vector<DeletionEntry> m_deletion_entries{};

public:
    void markForDeletion(DeletionEntry entry) { m_deletion_entries.push_back(entry); }

    // and of course a method here for deleting objects that are not in use
};

template <typename T>
class GPUResource : public T {
    VkQueue m_queue_using_resource = VK_NULL_HANDLE;
    uint64_t m_resource_free_signal_value = 0; // timeline semaphore associated with above queue
    GPUResourceDeleter<T>* const m_deleter;

protected:
    GPUResource(GPUResourceDeleter* deleter) : m_deleter(deleter) {}
    GPUResource(const GPUResource&) = delete;

    GPUResource& operator=(const GPUResource&) = delete;

    ~GPUResource() { m_deleter->markForDeletion({*static_cast<T>(this), m_queue_using_resource, m_resource_free_signal_value}); }

public:
    void useResource(VkQueue queue, uint64_t resource_free_signal_value)
    {
        m_queue_using_resource = queue;
        m_resource_free_signal_value = resource_free_signal_value;
    }
};

class RenderBackend {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    // global descriptor pool
    VkDescriptorPool m_main_desciptor_pool{};

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
    explicit RenderBackend(SDL_Window* window_handle);
    RenderBackend(const RenderBackend&) = delete;

    ~RenderBackend();

    RenderBackend operator=(const RenderBackend&) = delete;

    /* Renders to framebuffer and presents framebuffer to the screen */
    void submitFrame(bool window_resized, const WorldDrawData& world_draw_data);

    Pipeline createPipeline();

    RenderBackendInfo getInfo() const
    {
        RenderBackendInfo info{};
        info.instance = m_device.getInstance();
        info.device = m_device.getHandle();
        info.physical_device = m_device.getPhysicalDevice();
        info.main_queue = m_device.getMainQueue();
        info.main_queue_family_index = m_device.getMainQueueFamilyIndex();
        info.main_descriptor_pool = m_main_desciptor_pool;
        info.framebuffer_format = m_swapchain.getSurfaceFormat().format;
        info.depth_stencil_format = m_depth_stencil_format;
        return info;
    }

    void waitIdle();

private:
    void recreateFramesInFlightResources();
};

} // namespace gc
