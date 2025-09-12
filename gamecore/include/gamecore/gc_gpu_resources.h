#pragma once

#include <vector>
#include <span>
#include <functional>

#include "gamecore/gc_vulkan_common.h"

/*
 * Defines the GPUResource base class to be used for all resources that must
 * wait until they are not in use by a GPU queue before being destroyed.
 */

namespace gc {

class GPUResourceDeleteQueue {
public:
    struct DeletionEntry {
        VkSemaphore timeline_semaphore;        // the timeline semaphore corresponding to the queue using the resource
        uint64_t resource_free_signal_value;   // when the resource can be safely destroyed
        std::function<void(VkDevice)> deleter; // typically stores the resource handle as a lambda capture and calls vkDestroyXXX();
    };

private:
    VkDevice m_device{};
    std::vector<DeletionEntry> m_deletion_entries{};

public:
    GPUResourceDeleteQueue(VkDevice device) : m_device(device) {}

    /* Mark a GPU resource for deletion. Should be called in the destructor of the derived class. */
    void markForDeletion(const DeletionEntry& entry) { m_deletion_entries.push_back(entry); }

    /* Deletes all resources that are no longer in use by calling the corresponding deleter function object. */
    /* 'timeline_semaphores' should be the corresponding timeline semaphore for every queue that uses GPUResources.  */
    void deleteUnusedResources(std::span<const VkSemaphore> timeline_semaphores)
    {
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
                        GC_TRACE("Deleting a GPU resource");
                        m_deletion_entries[j].deleter(m_device);
                        m_deletion_entries[j] = m_deletion_entries.back();
                        m_deletion_entries.pop_back();
                    }
                }
            }
        }
    }

    bool empty() const { return m_deletion_entries.empty(); }
};

// Ensure that derived classes using this class call m_delete_queue->markForDeletion()
class GPUResource {
    VkSemaphore m_timeline_semaphore = VK_NULL_HANDLE; // timeline semaphore associated with the queue this resource was last used with
    uint64_t m_resource_free_signal_value = 0; // when the resource is no longer in use
    GPUResourceDeleteQueue& m_delete_queue; // points to the global GPUResource delete queue

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

    void markForDeletion(const std::function<void(VkDevice)>& deleter)
    {
        m_delete_queue.markForDeletion({m_timeline_semaphore, m_resource_free_signal_value, deleter});
    }

public:
    /* This should be called whenever the resource is used in a GPU queue */
    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        m_timeline_semaphore = timeline_semaphore;
        m_resource_free_signal_value = resource_free_signal_value;
    }
};

class GPUPipeline : public GPUResource {
public:
    VkPipeline handle;

public:
    GPUPipeline(GPUResourceDeleteQueue& delete_queue, VkPipeline pipeline) : GPUResource(delete_queue), handle(pipeline) {}
    GPUPipeline(const GPUPipeline&) = delete;
    GPUPipeline(GPUPipeline&& other) noexcept : GPUResource(std::move(other)), handle(other.handle) { other.handle = VK_NULL_HANDLE; }

    GPUPipeline& operator=(const GPUPipeline&) = delete;
    GPUPipeline& operator=(GPUPipeline&&) = delete;

    ~GPUPipeline()
    {
        if (handle != VK_NULL_HANDLE) {
            auto pipeline = handle;
            markForDeletion([pipeline](VkDevice device) { vkDestroyPipeline(device, pipeline, nullptr); });
        }
    }
};

} // namespace gc