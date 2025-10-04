#include "gamecore/gc_render_mesh.h"

namespace gc {

void RenderMesh::draw(VkCommandBuffer cmd, VkSemaphore timeline_semaphore, uint64_t signal_value)
{
    GC_ASSERT(cmd);
    GC_ASSERT(timeline_semaphore);

    const VkDeviceSize vertices_offset{0};
    const VkBuffer buffer = m_vertex_index_buffer.getHandle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vertices_offset);
    vkCmdBindIndexBuffer(cmd, buffer, m_indices_offset, m_index_type);

    vkCmdDrawIndexed(cmd, m_num_indices, 1, 0, 0, 0);

    m_vertex_index_buffer.useResource(timeline_semaphore, signal_value);
}

} // namespace gc