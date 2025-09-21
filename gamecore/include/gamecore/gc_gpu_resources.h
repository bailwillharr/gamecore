#pragma once

#include <vector>
#include <span>
#include <memory>
#include <functional>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_assert.h"

/*
 * Defines the GPUResource base class to be used for all resources that must
 * wait until they are not in use by a GPU queue before being destroyed.
 */

namespace gc {

class GPUResourceDeleteQueue {
public:
    struct DeletionEntry {
        VkSemaphore timeline_semaphore;                      // the timeline semaphore corresponding to the queue using the resource
        uint64_t resource_free_signal_value;                 // when the resource can be safely destroyed
        std::function<void(VkDevice, VmaAllocator)> deleter; // typically stores the resource handle as a lambda capture and calls vkDestroyXXX();
    };

private:
    VkDevice m_device{};
    VmaAllocator m_allocator{};
    std::vector<DeletionEntry> m_deletion_entries{};

public:
    GPUResourceDeleteQueue(VkDevice device, VmaAllocator allocator) : m_device(device), m_allocator(allocator) {}

    /* Mark a GPU resource for deletion. Should be called in the destructor of the derived class. */
    void markForDeletion(const DeletionEntry& entry) { m_deletion_entries.push_back(entry); }

    /* Deletes all resources that are no longer in use by calling the corresponding deleter function object. */
    /* 'timeline_semaphores' should be the corresponding timeline semaphore for every queue that uses GPUResources.  */
    /* Returns number of resources deleted */
    uint32_t deleteUnusedResources(std::span<const VkSemaphore> timeline_semaphores)
    {
        uint32_t num_resources_deleted{};
        if (!m_deletion_entries.empty()) { // very low cost function call if nothing to delete
            std::vector<uint64_t> timeline_values(timeline_semaphores.size());
            for (size_t i = 0; i < timeline_semaphores.size(); ++i) {
                GC_CHECKVK(vkGetSemaphoreCounterValue(m_device, timeline_semaphores[i], &timeline_values[i]));
                // iterate backwards, using ptrdiff_t because j must go negative:
                for (ptrdiff_t j = m_deletion_entries.size() - 1; j >= 0; --j) {
                    if ((m_deletion_entries[j].timeline_semaphore == timeline_semaphores[i] &&
                         timeline_values[i] >= m_deletion_entries[j].resource_free_signal_value) ||
                        m_deletion_entries[j].timeline_semaphore == VK_NULL_HANDLE) {
                        // if corresponding timeline semaphore has reached value yet
                        // OR
                        // resource had no timeline semaphore in which case always delete it
                        m_deletion_entries[j].deleter(m_device, m_allocator);
                        m_deletion_entries[j] = m_deletion_entries.back();
                        m_deletion_entries.pop_back();
                        ++num_resources_deleted;
                    }
                }
            }
        }
        return num_resources_deleted;
    }

    bool empty() const { return m_deletion_entries.empty(); }

    VkDevice getDevice() const { return m_device; }
};

// Ensure that derived classes using this class call m_delete_queue->markForDeletion()
class GPUResource {
    GPUResourceDeleteQueue& m_delete_queue;            // points to the global GPUResource delete queue
    VkSemaphore m_timeline_semaphore = VK_NULL_HANDLE; // timeline semaphore associated with the queue this resource was last used with
    uint64_t m_resource_free_signal_value = 0;         // when the resource is no longer in use

protected:
    GPUResource(GPUResourceDeleteQueue& delete_queue) : m_delete_queue(delete_queue) {}
    GPUResource(const GPUResource&) = delete;
    GPUResource(GPUResource&& other) noexcept
        : m_timeline_semaphore(other.m_timeline_semaphore),
          m_resource_free_signal_value(other.m_resource_free_signal_value),
          m_delete_queue(other.m_delete_queue)
    {
        // invalidate old object
        other.m_timeline_semaphore = VK_NULL_HANDLE;
        other.m_resource_free_signal_value = 0;
    }

    GPUResource& operator=(const GPUResource&) = delete;
    GPUResource& operator=(GPUResource&&) = delete;

    void markForDeletion(const std::function<void(VkDevice, VmaAllocator)>& deleter)
    {
        m_delete_queue.markForDeletion({m_timeline_semaphore, m_resource_free_signal_value, deleter});
    }

    VkSemaphore getTimelineSemaphore() const { return m_timeline_semaphore; }
    uint64_t getResourceFreeSignalValue() const { return m_resource_free_signal_value; }

public:
    /* This should be called whenever the resource is used in a GPU queue */
    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        m_timeline_semaphore = timeline_semaphore;
        m_resource_free_signal_value = resource_free_signal_value;
    }

    /* Returns true if the resource isn't in use by any queue. */
    bool isFree() const
    {
        if (m_timeline_semaphore == VK_NULL_HANDLE) {
            return true;
        }
        uint64_t current_semaphore_value{};
        GC_CHECKVK(vkGetSemaphoreCounterValue(m_delete_queue.getDevice(), m_timeline_semaphore, &current_semaphore_value));
        if (current_semaphore_value >= m_resource_free_signal_value) {
            return true;
        }
        return false;
    }
};

class GPUPipeline : public GPUResource {
    VkPipeline m_handle;

public:
    GPUPipeline(GPUResourceDeleteQueue& delete_queue, VkPipeline handle) : GPUResource(delete_queue), m_handle(handle) { GC_ASSERT(m_handle); }
    GPUPipeline(const GPUPipeline&) = delete;
    GPUPipeline(GPUPipeline&& other) noexcept : GPUResource(std::move(other)), m_handle(other.m_handle) { other.m_handle = VK_NULL_HANDLE; }

    GPUPipeline& operator=(const GPUPipeline&) = delete;
    GPUPipeline& operator=(GPUPipeline&&) = delete;

    ~GPUPipeline()
    {
        GC_TRACE("~GPUPipeline() {}", reinterpret_cast<void*>(m_handle));
        if (m_handle != VK_NULL_HANDLE) {
            auto pipeline = m_handle;
            markForDeletion([pipeline](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
                GC_TRACE("Deleting GPUPipeline {}", reinterpret_cast<void*>(pipeline));
                vkDestroyPipeline(device, pipeline, nullptr);
            });
        }
    }

    VkPipeline getHandle() const { return m_handle; }
};

/* A device-local GPU Image. Not owned by textures but usually owned by one or more GPUImageViews. */
/* Call useResource() when uploading the image otherwise isUploaded() won't work */
class GPUImage : public GPUResource {
    VkImage m_handle{};
    VmaAllocation m_allocation{};

public:
    GPUImage(GPUResourceDeleteQueue& delete_queue, VkImage handle, VmaAllocation allocation)
        : GPUResource(delete_queue), m_handle(handle), m_allocation(allocation)
    {
        GC_ASSERT(m_handle);
        GC_ASSERT(m_allocation);
    }
    GPUImage(const GPUImage&) = delete;
    GPUImage(GPUImage&& other) noexcept : GPUResource(std::move(other)), m_handle(other.m_handle), m_allocation(other.m_allocation)
    {
        other.m_handle = VK_NULL_HANDLE;
        other.m_allocation = {};
    }

    GPUImage& operator=(const GPUImage&) = delete;
    GPUImage& operator=(GPUImage&&) = delete;

    ~GPUImage()
    {
        GC_TRACE("~GPUImage() {}", reinterpret_cast<void*>(m_handle));
        if (m_handle != VK_NULL_HANDLE) {
            auto image = m_handle;
            auto allocation = m_allocation;
            markForDeletion([image, allocation]([[maybe_unused]] VkDevice device, VmaAllocator allocator) {
                GC_TRACE("Deleting GPUImage {}", reinterpret_cast<void*>(image));
                vmaDestroyImage(allocator, image, allocation);
            });
        }
    }

    VkImage getHandle() const { return m_handle; }
};

/* Ensure that image_view.getImage()->isUploaded() == true before using */
class GPUImageView : public GPUResource {
    VkImageView m_handle{};
    std::shared_ptr<GPUImage> m_image{};

public:
    GPUImageView(GPUResourceDeleteQueue& delete_queue, VkImageView handle, const std::shared_ptr<GPUImage>& image)
        : GPUResource(delete_queue), m_handle(handle), m_image(image)
    {
        GC_ASSERT(m_handle);
        GC_ASSERT(m_image);
    }
    GPUImageView(const GPUImageView&) = delete;
    GPUImageView(GPUImageView&& other) noexcept : GPUResource(std::move(other)), m_handle(other.m_handle), m_image(other.m_image)
    {
        other.m_handle = VK_NULL_HANDLE;
        other.m_image = nullptr;
    }

    GPUImageView& operator=(const GPUImageView&) = delete;
    GPUImageView& operator=(GPUImageView&&) = delete;

    ~GPUImageView()
    {
        GC_TRACE("~GPUImageView() {}", reinterpret_cast<void*>(m_handle));
        if (m_handle != VK_NULL_HANDLE) {
            auto image_view = m_handle;
            markForDeletion([image_view](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
                GC_TRACE("Deleting GPUImageView: {}", reinterpret_cast<void*>(image_view));
                vkDestroyImageView(device, image_view, nullptr);
            });
        }
    }

    /* Also calls useResource() on the image */
    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        GC_ASSERT(m_image);
        m_image->useResource(timeline_semaphore, resource_free_signal_value);
        GPUResource::useResource(timeline_semaphore, resource_free_signal_value);
    }

    const std::shared_ptr<GPUImage>& getImage() const
    {
        GC_ASSERT(m_image);
        return m_image;
    }
};

/* A buffer. Could be a host-local mapped staging buffer, a vertex buffer, whatever. */
class GPUBuffer : public GPUResource {
    VkBuffer m_handle{};
    VmaAllocation m_allocation{};

public:
    GPUBuffer(GPUResourceDeleteQueue& delete_queue, VkBuffer handle, VmaAllocation allocation)
        : GPUResource(delete_queue), m_handle(handle), m_allocation(allocation)
    {
        GC_ASSERT(m_handle);
        GC_ASSERT(m_allocation);
    }
    GPUBuffer(const GPUBuffer&) = delete;
    GPUBuffer(GPUBuffer&& other) noexcept : GPUResource(std::move(other)), m_handle(other.m_handle), m_allocation(other.m_allocation)
    {
        other.m_handle = VK_NULL_HANDLE;
        other.m_allocation = {};
    }

    GPUBuffer& operator=(const GPUBuffer&) = delete;
    GPUBuffer& operator=(GPUBuffer&&) = delete;

    ~GPUBuffer()
    {
        GC_TRACE("~GPUStagingBuffer() {}", reinterpret_cast<void*>(m_handle));
        if (m_handle != VK_NULL_HANDLE) {
            auto buffer = m_handle;
            auto allocation = m_allocation;
            markForDeletion([buffer, allocation]([[maybe_unused]] VkDevice device, VmaAllocator allocator) {
                GC_TRACE("Deleting GPUStagingBuffer {}", reinterpret_cast<void*>(buffer));
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
        }
    }

    VkBuffer getHandle() const { return m_handle; }
};

} // namespace gc
