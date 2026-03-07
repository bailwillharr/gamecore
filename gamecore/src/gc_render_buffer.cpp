#include "gamecore/gc_render_buffer.h"

#include <tuple>

namespace gc {

static std::pair<VkBuffer, VmaAllocation> createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, bool mapped, void** mapping)
{
    if (mapped) {
        GC_ASSERT(mapping);
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo buffer_alloc_info{};
    buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (mapped) {
        buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    buffer_alloc_info.priority = 0.5f;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo allocation_mapping_info{};
    GC_CHECKVK(vmaCreateBuffer(allocator, &buffer_info, &buffer_alloc_info, &buffer, &allocation, &allocation_mapping_info));

    if (mapped) {
        GC_ASSERT(allocation_mapping_info.pMappedData);
        *mapping = allocation_mapping_info.pMappedData;
    }

    return std::make_pair(buffer, allocation);
}

RenderBuffer::RenderBuffer(GPUResourceDeleteQueue& delete_queue, VmaAllocator allocator, uint32_t frames_in_flight, VkDeviceSize size, VkBufferUsageFlags usage)
    : m_delete_queue(delete_queue), m_allocator(allocator), m_usage(usage), m_frames_in_flight(frames_in_flight)
{
    GC_ASSERT(allocator);
    GC_ASSERT(frames_in_flight > 0);
    GC_ASSERT(size != 0);

    m_staging_buffers.resize(frames_in_flight);

    reallocate(size);
}

void RenderBuffer::writeData(VkCommandBuffer cmd, uint64_t current_frame_index, VkSemaphore timeline_semaphore, uint64_t signal_value,
                             std::span<const uint8_t> data)
{
    m_size = data.size();
    if (m_size > m_capacity) {
        reallocate(std::max(m_size, m_capacity * 2));
    }

    auto& staging_buffer = m_staging_buffers[current_frame_index % m_frames_in_flight];

    memcpy(staging_buffer.mapping, data.data(), data.size());
    vmaFlushAllocation(m_allocator, staging_buffer.allocation, 0, m_size);

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = m_size;

    vkCmdCopyBuffer(cmd, staging_buffer.buffer, m_buffer, 1, &region);

    m_timeline_semaphore = timeline_semaphore;
    m_resource_free_signal_value = signal_value;
}

VkBuffer RenderBuffer::getBuffer() const { return m_buffer; }

void RenderBuffer::reallocate(VkDeviceSize new_capacity)
{
    m_capacity = new_capacity;

    if (m_buffer) {
        GPUResourceDeleteQueue::DeletionEntry entry{};
        entry.timeline_semaphore = m_timeline_semaphore;
        entry.resource_free_signal_value = m_resource_free_signal_value;
        entry.deleter = [buffer = m_buffer, allocation = m_allocation](VkDevice, VmaAllocator allocator) { vmaDestroyBuffer(allocator, buffer, allocation); };
        m_delete_queue.markForDeletion(entry);
    }

    std::tie(m_buffer, m_allocation) = createBuffer(m_allocator, m_capacity, VK_BUFFER_USAGE_TRANSFER_DST_BIT | m_usage, false, nullptr);

    for (auto& staging_buffer : m_staging_buffers) {

        // the wait here is course, one of the staging buffers will almost certainly be free to delete right away.
        if (staging_buffer.buffer) {
            GPUResourceDeleteQueue::DeletionEntry entry{};
            entry.timeline_semaphore = m_timeline_semaphore;
            entry.resource_free_signal_value = m_resource_free_signal_value;
            entry.deleter = [buffer = staging_buffer.buffer, allocation = staging_buffer.allocation](VkDevice, VmaAllocator allocator) {
                vmaDestroyBuffer(allocator, buffer, allocation);
            };
            m_delete_queue.markForDeletion(entry);
        }

        void* mapping{};
        std::tie(staging_buffer.buffer, staging_buffer.allocation) = createBuffer(m_allocator, m_capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &mapping);
        staging_buffer.mapping = mapping;
    }
}

} // namespace gc