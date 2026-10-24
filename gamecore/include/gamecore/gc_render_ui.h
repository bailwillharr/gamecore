#pragma once

#include <array>
#include <memory>

#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

class UIDrawData;   // forward-dec
class RenderBuffer; // forward-dec

// A texture that the UI draws with (see UIDrawData): an image (RGBA8, sRGB, alpha premultiplied) and the descriptor set that it
// is bound with. Made by RenderBackend::createUITexture().
class RenderUITexture {
    RenderTexture m_texture;
    GPUDescriptorSet m_descriptor_set;
    bool m_descriptor_written{};

public:
    RenderUITexture(RenderTexture&& texture, GPUDescriptorSet&& descriptor_set);
    RenderUITexture(const RenderUITexture&) = delete;
    RenderUITexture(RenderUITexture&&) = delete;

    ~RenderUITexture();

    RenderUITexture& operator=(const RenderUITexture&) = delete;
    RenderUITexture& operator=(RenderUITexture&&) = delete;

    // For the RenderBackend, when it is about to draw with the texture. UI textures are small, and are often made in the frame
    // that first draws them (such as a font's glyphs), so this waits for the upload rather than leaving the texture out.
    // Then it points the descriptor set at the image, the first time.
    VkDescriptorSet prepare(VkDevice device);

    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value);
};

// The pipelines that the UI is drawn with, indexed by this
enum class UIPipelineKind : uint32_t {
    DRAW,              // blends (alpha premultiplied)
    DRAW_CLIPPED,      // the same, where the stencil buffer is the reference value
    CLIP_REPLACE,      // writes the reference value to the stencil buffer, and no color
    CLIP_INCREMENT,    // adds 1 to the stencil buffer, and writes no color
    COUNT,
};

// To be called in a render pass instance with a stencil attachment. The vertices and indices of draw_data have already been
// copied into the buffers. white_texture is bound for draws that have no texture.
// Sets the viewport and the scissor, and leaves them set to whatever the UI last used.
void recordUIRenderingCommands(VkCommandBuffer cmd, VkDevice device, VkPipelineLayout pipeline_layout,
                               const std::array<std::unique_ptr<GPUPipeline>, static_cast<size_t>(UIPipelineKind::COUNT)>& pipelines,
                               RenderUITexture& white_texture, RenderBuffer& vertex_buffer, RenderBuffer& index_buffer, VkExtent2D extent,
                               VkSemaphore timeline_semaphore, uint64_t signal_value, const UIDrawData& draw_data);

} // namespace gc
