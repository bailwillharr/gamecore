#pragma once

#include <span>

#include <vec2.hpp>
#include <vec3.hpp>
#include <vec4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_assert.h"

namespace gc {

struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 uv;
};

class RenderMesh {
    GPUBuffer m_vertex_index_buffer;
    const VkDeviceSize m_indices_offset;
    const VkIndexType m_index_type;
    const uint32_t m_num_indices;
    mutable bool m_uploaded{false};

public:
    RenderMesh(GPUBuffer&& vertex_index_buffer, VkDeviceSize indices_offset, VkIndexType index_type, uint32_t num_indices)
        : m_vertex_index_buffer(std::move(vertex_index_buffer)), m_indices_offset(indices_offset), m_index_type(index_type), m_num_indices(num_indices)
    {
        GC_ASSERT(m_indices_offset > 0ULL);
        GC_ASSERT(m_index_type == VK_INDEX_TYPE_UINT16 || m_index_type == VK_INDEX_TYPE_UINT32);
    }

    bool isUploaded() const
    {
        if (m_uploaded) {
            return true;
        }
        // if the buffer is no longer in use by the queue, assuming the buffer was just created, this means the buffer is uploaded.
        if (m_vertex_index_buffer.isFree()) {
            m_uploaded = true;
            return true;
        }
        return false;
    }

    // Ensure isUploaded() returned true before calling this
    void draw(VkCommandBuffer cmd, VkSemaphore timeline_semaphore, uint64_t signal_value);
};

} // namespace gc