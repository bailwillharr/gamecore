#pragma once

// The GPU's side of a terrain (see gc_terrain.h): a cache of its tiles, and what a patch of it is drawn with.
//
// The cache is one storage buffer with room for a fixed number of tiles, in slots. Whoever draws the terrain (the TerrainSystem)
// decides which tiles are in which slots: it copies a tile into a free slot when it is needed, and lets the slot go again when
// the tile hasn't been needed for a while and the slot is wanted for another. A slot can only be given another tile once the GPU
// has finished with it: both the copy into it and the frames that drew from it (isSlotFree()).
//
// Every patch of the terrain is the same grid of vertices, whose heights the vertex shader reads from a tile, so the whole
// terrain is one instanced draw of that grid (see TerrainPatch). The grid has no vertex buffer: the vertex shader works out
// where each vertex is from its index. It has an extra ring of vertices around it for its skirts.

#include <cstdint>

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_uploader.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

struct TerrainPatch; // forward-dec

// The terrain vertex shader's push constants, which are after the world matrix and the material's constants (see
// RenderBackend::setTerrainShaders())
struct TerrainPushConstants {
    float height_offset;
    float height_scale;
    uint32_t tile_side;   // samples along a tile's side
    uint32_t patch_quads; // quads along a patch's side
};
constexpr uint32_t TERRAIN_PUSH_CONSTANT_OFFSET = 112; // (a vec4 in the shader, so on a 16 byte boundary)

class RenderTerrain {
    struct Slot {
        std::optional<RenderUpload> upload{}; // of the tile that is being copied into the slot, until the GPU has been told to
        GPUUseTracker copy;                   // in use until the GPU has copied the tile in
        GPUUseTracker draws;                  // in use until the GPU has finished the frames that drew from the slot
        bool loading{false};                  // a tile is being copied into the slot
        bool uploaded{false};                 // the slot has a tile, which has been copied

        explicit Slot(GPUResourceDeleteQueue& delete_queue) : copy(delete_queue), draws(delete_queue) {}
    };

    RenderUploader& m_uploader;
    const uint32_t m_tile_quads;
    const uint32_t m_tile_sample_count; // (quads + 1)^2
    GPUBuffer m_tiles;                  // every slot, one after another
    GPUBuffer m_indices;                // the patch's grid
    std::optional<RenderUpload> m_indices_upload{};
    const uint32_t m_index_count;
    GPUDescriptorSet m_descriptor_set; // the tiles, for the vertex shader
    std::vector<Slot> m_slots{};
    bool m_ready{false};

public:
    // The vertices of a patch's grid along one side, including its skirts
    static uint32_t getPatchVertexSide(uint32_t tile_quads) { return tile_quads / 2 + 3; }
    // The indices of the grid. Each patch is a quarter of a tile
    static std::vector<uint32_t> makePatchIndices(uint32_t tile_quads);

    // Made by RenderBackend::createTerrain()
    RenderTerrain(RenderUploader& uploader, GPUResourceDeleteQueue& delete_queue, uint32_t tile_quads, uint32_t slot_count, GPUBuffer&& tiles,
                  GPUBuffer&& indices, RenderUpload&& indices_upload, uint32_t index_count, GPUDescriptorSet&& descriptor_set);
    RenderTerrain(const RenderTerrain&) = delete;
    RenderTerrain(RenderTerrain&&) = default;

    // Waits for the copies into the slots that the GPU has been given
    ~RenderTerrain();

    RenderTerrain& operator=(const RenderTerrain&) = delete;
    RenderTerrain& operator=(RenderTerrain&&) = delete;

    uint32_t getSlotCount() const { return static_cast<uint32_t>(m_slots.size()); }
    uint32_t getTileQuads() const { return m_tile_quads; }
    // Where a slot's tile is in the buffer, in samples, as in TerrainPatch::tile
    uint32_t getTileOffset(uint32_t slot) const { return slot * m_tile_sample_count; }

    // Starts copying a tile ((quads + 1)^2 samples) into a slot. The slot must be free (isSlotFree()). The data is read later if the
    // mode is BACKGROUND (see RenderUploadMode). A tile that was in the slot is forgotten.
    void uploadTile(uint32_t slot, std::span<const uint32_t> samples, RenderUploadMode mode);

    // Whether the tile that was last put in the slot has been copied, so that it can be drawn
    bool isTileUploaded(uint32_t slot);

    // Whether a slot can be given another tile: the GPU isn't copying into it or drawing from it. A copy that the GPU hasn't been
    // given yet doesn't count: uploading another tile cancels it.
    bool isSlotFree(uint32_t slot);

    // Forgets the tile in a slot. (Nothing happens to the buffer: the slot just isn't uploaded any more.)
    void evictTile(uint32_t slot);

    // Whether the patch grid has been uploaded, so that the terrain can be drawn
    bool isReady();

    // For RenderBackend: binds the tiles (descriptor set 4) and the patch grid's index buffer, and marks what the patches read as
    // in use until the frame is done.
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value);
    void markDrawn(std::span<const TerrainPatch> patches, VkSemaphore timeline_semaphore, uint64_t signal_value);
    uint32_t getIndexCount() const { return m_index_count; }

private:
    void finishSubmittedUpload(Slot& slot);
};

} // namespace gc
