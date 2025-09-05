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
#include <functional>

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

class GPUResourceDeleteQueue {
public:
    struct DeletionEntry {
        VkSemaphore timeline_semaphore;
        uint64_t resource_free_signal_value;
        std::function<void(VkDevice)> deleter;
    };

private:
    std::vector<DeletionEntry> m_deletion_entries{};

public:
    void markForDeletion(DeletionEntry entry) { m_deletion_entries.push_back(entry); }

    void deleteUnusedResources(VkDevice device, const std::vector<VkSemaphore>& semaphores)
    {
        if (!m_deletion_entries.empty()) { // very low cost function call if nothing to delete
            std::vector<uint64_t> timeline_values(semaphores.size());
            for (size_t i = 0; i < semaphores.size(); ++i) {
                GC_CHECKVK(vkGetSemaphoreCounterValue(device, semaphores[i], &timeline_values[i]));
                // iterate backwards:
                for (size_t j = m_deletion_entries.size() - 1; j >= 0; --j) {
                    if (m_deletion_entries[j].timeline_semaphore == semaphores[i] && timeline_values[i] >= m_deletion_entries[j].resource_free_signal_value) {
                        m_deletion_entries[j].deleter(device);
                        m_deletion_entries[j] = m_deletion_entries.back();
                        m_deletion_entries.pop_back();
                    }
                }
            }
        }
    }
};

// Ensure that derived classes using this class call m_delete_queue->markForDeletion()
class GPUResource {
    VkSemaphore m_timeline_semaphore = VK_NULL_HANDLE; // timeline semaphore associated with the queue this resource was last used with
    uint64_t m_resource_free_signal_value = 0;
    GPUResourceDeleteQueue& m_delete_queue;

protected:
    GPUResource(GPUResourceDeleteQueue& delete_queue) : m_delete_queue(delete_queue) {}
    GPUResource(const GPUResource&) = delete;

    GPUResource& operator=(const GPUResource&) = delete;

    void markForDeletion(const std::function<void(VkDevice)>& deleter)
    {
        m_delete_queue.markForDeletion({m_timeline_semaphore, m_resource_free_signal_value, deleter});
    }

public:
    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        m_timeline_semaphore = timeline_semaphore;
        m_resource_free_signal_value = resource_free_signal_value;
    }
};

class Pipeline : public GPUResource {
    VkPipeline m_pipeline;
    VkPipelineLayout m_layout;

public:
    Pipeline(VkPipeline pipeline, VkPipelineLayout layout, GPUResourceDeleteQueue& delete_queue) : GPUResource(delete_queue) {}
    ~Pipeline()
    {
        auto pipeline = m_pipeline;
        auto layout = m_layout;
        markForDeletion([pipeline, layout](VkDevice device) {
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
        });
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

    GPUResourceDeleteQueue m_delete_queue{};

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
