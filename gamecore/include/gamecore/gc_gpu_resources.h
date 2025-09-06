#pragma once

#include <vector>
#include <span>
#include <functional>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

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

    void deleteUnusedResources(VkDevice device, std::span<const VkSemaphore> semaphores)
    {
        if (!m_deletion_entries.empty()) { // very low cost function call if nothing to delete
            std::vector<uint64_t> timeline_values(semaphores.size());
            for (size_t i = 0; i < semaphores.size(); ++i) {
                GC_CHECKVK(vkGetSemaphoreCounterValue(device, semaphores[i], &timeline_values[i]));
                // iterate backwards, using ptrdiff_t because j must go negative:
                for (ptrdiff_t j = m_deletion_entries.size() - 1; j >= 0; --j) {
                    if ((m_deletion_entries[j].timeline_semaphore == semaphores[i] && timeline_values[i] >= m_deletion_entries[j].resource_free_signal_value) ||
                        m_deletion_entries[j].timeline_semaphore == VK_NULL_HANDLE) {
                        // if corresponding timeline semaphore has reached value yet
                        // OR
                        // resource had no timeline semaphore in which case always delete it
                        GC_TRACE("Deleting a GPU resource");
                        m_deletion_entries[j].deleter(device);
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

class GPUPipeline : public GPUResource {
public:
    const VkPipeline handle;

public:
    GPUPipeline(GPUResourceDeleteQueue& delete_queue, VkPipeline pipeline) : GPUResource(delete_queue), handle(pipeline) {}
    ~GPUPipeline()
    {
        auto pipeline = handle;
        markForDeletion([pipeline](VkDevice device) { vkDestroyPipeline(device, pipeline, nullptr); });
    }
};

} // namespace gc