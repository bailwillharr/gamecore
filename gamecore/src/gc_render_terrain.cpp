#include "gamecore/gc_render_terrain.h"

#include "gamecore/gc_assert.h"
#include "gamecore/gc_world_draw_data.h"

namespace gc {

std::vector<uint32_t> RenderTerrain::makePatchIndices(uint32_t tile_quads)
{
    // A patch is a quarter of a tile: half of its quads along each side. Around them is a ring of vertices that the vertex shader
    // puts in the same places as the patch's edge, but lower down: the skirts.
    const uint32_t side = getPatchVertexSide(tile_quads);
    const auto vertex = [side](uint32_t x, uint32_t y) { return y * side + x; };

    std::vector<uint32_t> indices{};
    indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6 + static_cast<size_t>(side) * 4 * 6);
    for (uint32_t y = 0; y + 1 < side; ++y) {
        for (uint32_t x = 0; x + 1 < side; ++x) {
            const bool skirt_x = (x == 0 || x + 2 == side);
            const bool skirt_y = (y == 0 || y + 2 == side);
            if (skirt_x && skirt_y) {
                continue; // a corner of the ring, which is all in one place
            }
            const uint32_t v00 = vertex(x, y);
            const uint32_t v10 = vertex(x + 1, y);
            const uint32_t v01 = vertex(x, y + 1);
            const uint32_t v11 = vertex(x + 1, y + 1);
            // Split along the diagonal from (0, 0) to (1, 1), as the terrain's collision and its levels of detail expect.
            // Counter-clockwise from above.
            indices.insert(indices.end(), {v00, v10, v11, v00, v11, v01});
            if (skirt_x || skirt_y) {
                // A skirt is a wall, which could be seen from either side, so it faces both ways
                indices.insert(indices.end(), {v00, v11, v10, v00, v01, v11});
            }
        }
    }
    return indices;
}

RenderTerrain::RenderTerrain(RenderUploader& uploader, GPUResourceDeleteQueue& delete_queue, uint32_t tile_quads, uint32_t slot_count, GPUBuffer&& tiles,
                             GPUBuffer&& indices, RenderUpload&& indices_upload, uint32_t index_count, GPUDescriptorSet&& descriptor_set)
    : m_uploader(uploader),
      m_tile_quads(tile_quads),
      m_tile_sample_count((tile_quads + 1) * (tile_quads + 1)),
      m_tiles(std::move(tiles)),
      m_indices(std::move(indices)),
      m_index_count(index_count),
      m_descriptor_set(std::move(descriptor_set))
{
    m_indices_upload.emplace(std::move(indices_upload));
    m_slots.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        m_slots.emplace_back(delete_queue);
    }
    GC_TRACE("Created RenderTerrain");
}

RenderTerrain::~RenderTerrain()
{
    GC_TRACE("Destroying RenderTerrain...");
    // The buffer is only marked as in use by the frames that drew from it, so wait for the copies into it here. (Copies that the GPU
    // hasn't been given are cancelled when the slots are destroyed.)
    for (Slot& slot : m_slots) {
        finishSubmittedUpload(slot);
        slot.copy.waitForFree();
    }
    if (m_indices_upload && m_indices_upload->isSubmitted()) {
        m_indices_upload->finishBuffer(m_indices);
    }
}

void RenderTerrain::uploadTile(uint32_t slot, std::span<const uint32_t> samples, RenderUploadMode mode)
{
    GC_ASSERT(slot < m_slots.size());
    GC_ASSERT(samples.size() == m_tile_sample_count);
    Slot& s = m_slots[slot];
    finishSubmittedUpload(s);
    GC_ASSERT(s.copy.isFree() && s.draws.isFree());

    RenderUploader::Sources sources{};
    sources.push_back(std::span(reinterpret_cast<const uint8_t*>(samples.data()), samples.size_bytes()));
    const VkDeviceSize offset = static_cast<VkDeviceSize>(getTileOffset(slot)) * sizeof(uint32_t);
    s.upload.reset(); // (cancels a copy that the GPU hasn't been given)
    s.upload.emplace(m_uploader.uploadBuffer(m_tiles.getHandle(), sources, mode, offset));
    s.loading = true;
    s.uploaded = false;
}

bool RenderTerrain::isTileUploaded(uint32_t slot)
{
    GC_ASSERT(slot < m_slots.size());
    Slot& s = m_slots[slot];
    if (s.uploaded) {
        return true;
    }
    if (!s.loading) {
        return false; // nothing was put in the slot, or it was evicted
    }
    finishSubmittedUpload(s);
    // (the upload is let go once the GPU has been given it, and the copy is done once the GPU has finished it)
    if (!s.upload && s.copy.isFree()) {
        s.loading = false;
        s.uploaded = true;
    }
    return s.uploaded;
}

bool RenderTerrain::isSlotFree(uint32_t slot)
{
    GC_ASSERT(slot < m_slots.size());
    Slot& s = m_slots[slot];
    finishSubmittedUpload(s);
    return s.copy.isFree() && s.draws.isFree();
}

void RenderTerrain::evictTile(uint32_t slot)
{
    GC_ASSERT(slot < m_slots.size());
    Slot& s = m_slots[slot];
    finishSubmittedUpload(s);
    s.upload.reset();
    s.loading = false;
    s.uploaded = false;
}

bool RenderTerrain::isReady()
{
    if (!m_ready) {
        if (m_indices_upload && m_indices_upload->isSubmitted()) {
            m_indices_upload->finishBuffer(m_indices);
            m_indices_upload.reset();
        }
        m_ready = !m_indices_upload && m_indices.isFree();
    }
    return m_ready;
}

void RenderTerrain::bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value)
{
    GC_ASSERT(m_ready);
    m_descriptor_set.useResource(timeline_semaphore, signal_value);
    m_tiles.useResource(timeline_semaphore, signal_value);
    m_indices.useResource(timeline_semaphore, signal_value);
    const VkDescriptorSet set = m_descriptor_set.getHandle();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 4, 1, &set, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, m_indices.getHandle(), 0, VK_INDEX_TYPE_UINT32);
}

void RenderTerrain::markDrawn(std::span<const TerrainPatch> patches, VkSemaphore timeline_semaphore, uint64_t signal_value)
{
    for (const TerrainPatch& patch : patches) {
        const uint32_t slot = patch.tile / m_tile_sample_count;
        const uint32_t parent_slot = patch.parent_tile / m_tile_sample_count;
        GC_ASSERT(slot < m_slots.size() && parent_slot < m_slots.size());
        m_slots[slot].draws.useResource(timeline_semaphore, signal_value);
        m_slots[parent_slot].draws.useResource(timeline_semaphore, signal_value);
    }
}

void RenderTerrain::finishSubmittedUpload(Slot& slot)
{
    if (slot.upload && slot.upload->isSubmitted()) {
        slot.upload->finishBuffer(slot.copy);
        slot.upload.reset();
    }
}

} // namespace gc
