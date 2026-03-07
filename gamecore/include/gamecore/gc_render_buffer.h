#pragma once

#include <span>

#include <gamecore/gc_vulkan_common.h>
#include <gamecore/gc_vulkan_allocator.h>

#include <gctemplates/gct_static_vector.h>

#include "gamecore/gc_gpu_resources.h"

namespace gc {

class RenderBuffer {

    struct StagingBuffer {
        VkBuffer buffer;
        VmaAllocation allocation;
        void* mapping;
    };

    VkBuffer m_buffer{};
    VmaAllocation m_allocation{};

    GPUResourceDeleteQueue& m_delete_queue;

    gct::static_vector<StagingBuffer, 2> m_staging_buffers{};

    VkDeviceSize m_size{};
    VkDeviceSize m_capacity{};

    VkSemaphore m_timeline_semaphore{};
    uint64_t m_resource_free_signal_value{};

    const VmaAllocator m_allocator;

    const VkBufferUsageFlags m_usage;
    const uint32_t m_frames_in_flight;

public:
    RenderBuffer(GPUResourceDeleteQueue& delete_queue, VmaAllocator allocator, uint32_t frames_in_flight, VkDeviceSize size, VkBufferUsageFlags usage);

    // The staging buffer should not be in use on the GPU when this is called
    void writeData(VkCommandBuffer cmd, uint64_t current_frame_index, VkSemaphore timeline_semaphore, uint64_t signal_value, std::span<const uint8_t> data);

    VkBuffer getBuffer() const;

private:
    void reallocate(VkDeviceSize new_capacity);
};

} // namespace gc