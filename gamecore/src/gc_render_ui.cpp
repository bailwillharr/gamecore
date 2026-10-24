#include "gamecore/gc_render_ui.h"

#include <algorithm>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_render_buffer.h"
#include "gamecore/gc_ui_draw_data.h"

namespace gc {

// The push constants of content/shader_src/ui.vert and ui.frag
struct UIPushConstants {
    glm::mat4 transform; // from pixels to clip space
    glm::vec2 translation;
    uint32_t flags; // bit 0: sample the texture, bit 1: it's a gradient's colors
    uint32_t gradient_function;
    glm::vec2 gradient_p;
    glm::vec2 gradient_v;
    float first_stop;
    float last_stop;
    float ramp_start;
    float ramp_end;
};
static_assert(sizeof(UIPushConstants) == 112);

static constexpr uint32_t UI_FLAG_TEXTURE = 1;
static constexpr uint32_t UI_FLAG_GRADIENT = 2;

RenderUITexture::RenderUITexture(RenderTexture&& texture, GPUDescriptorSet&& descriptor_set)
    : m_texture(std::move(texture)), m_descriptor_set(std::move(descriptor_set))
{
}

RenderUITexture::~RenderUITexture() = default;

VkDescriptorSet RenderUITexture::prepare(VkDevice device)
{
    if (!m_descriptor_written) {
        m_texture.waitForUpload();

        VkDescriptorImageInfo image_info{};
        image_info.sampler = VK_NULL_HANDLE; // immutable sampler
        image_info.imageView = m_texture.getImageView();
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptor_set.getHandle();
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        m_descriptor_written = true;
    }
    return m_descriptor_set.getHandle();
}

void RenderUITexture::useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
{
    m_texture.useResource(timeline_semaphore, resource_free_signal_value);
    m_descriptor_set.useResource(timeline_semaphore, resource_free_signal_value);
}

void recordUIRenderingCommands(VkCommandBuffer cmd, VkDevice device, VkPipelineLayout pipeline_layout,
                               const std::array<std::unique_ptr<GPUPipeline>, static_cast<size_t>(UIPipelineKind::COUNT)>& pipelines,
                               RenderUITexture& white_texture, RenderBuffer& vertex_buffer, RenderBuffer& index_buffer, VkExtent2D extent,
                               VkSemaphore timeline_semaphore, uint64_t signal_value, const UIDrawData& draw_data)
{
    ZoneScoped;

    if (draw_data.empty() || extent.width == 0 || extent.height == 0) {
        return;
    }

    // From pixels (origin at the top left) to clip space. The UI was laid out on a screen of this size, which is the size of the
    // framebuffer except perhaps for a frame while the window is being resized
    glm::vec2 size = glm::vec2(draw_data.getSize());
    if (size.x <= 0.0f || size.y <= 0.0f) {
        size = glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    }
    glm::mat4 projection(1.0f);
    projection[0][0] = 2.0f / size.x;
    projection[1][1] = 2.0f / size.y;
    projection[3][0] = -1.0f;
    projection[3][1] = -1.0f;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const VkBuffer vertex_buffer_handle = vertex_buffer.getBuffer();
    const VkDeviceSize vertex_buffer_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_handle, &vertex_buffer_offset);
    vkCmdBindIndexBuffer(cmd, index_buffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);

    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    const auto& transforms = draw_data.getTransforms();

    for (const UIDrawCommand& command : draw_data.getCommands()) {
        UIPipelineKind pipeline_kind{};
        uint32_t stencil_reference = 0;
        bool clear_stencil = false;
        uint32_t stencil_clear_value = 0;
        switch (command.kind) {
        case UIDrawKind::TRIANGLES:
        case UIDrawKind::GRADIENT:
            pipeline_kind = command.use_clip_mask ? UIPipelineKind::DRAW_CLIPPED : UIPipelineKind::DRAW;
            stencil_reference = command.clip_mask_value;
            break;
        case UIDrawKind::CLIP_SET:
            pipeline_kind = UIPipelineKind::CLIP_REPLACE;
            clear_stencil = true;
            stencil_clear_value = 0;
            stencil_reference = 1;
            break;
        case UIDrawKind::CLIP_SET_INVERSE:
            pipeline_kind = UIPipelineKind::CLIP_REPLACE;
            clear_stencil = true;
            stencil_clear_value = 1;
            stencil_reference = 0;
            break;
        case UIDrawKind::CLIP_INTERSECT:
            pipeline_kind = UIPipelineKind::CLIP_INCREMENT;
            break;
        }

        if (clear_stencil) {
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            clear.clearValue.depthStencil.depth = 0.0f;
            clear.clearValue.depthStencil.stencil = stencil_clear_value;
            VkClearRect rect{};
            rect.rect.offset = {0, 0};
            rect.rect.extent = extent;
            rect.baseArrayLayer = 0;
            rect.layerCount = 1;
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }

        // the scissor, kept inside the framebuffer
        const int32_t x0 = std::clamp(command.scissor.x, 0, static_cast<int32_t>(extent.width));
        const int32_t y0 = std::clamp(command.scissor.y, 0, static_cast<int32_t>(extent.height));
        const int32_t x1 = std::clamp(command.scissor.x + command.scissor.z, x0, static_cast<int32_t>(extent.width));
        const int32_t y1 = std::clamp(command.scissor.y + command.scissor.w, y0, static_cast<int32_t>(extent.height));
        if (x1 == x0 || y1 == y0 || command.index_count == 0) {
            continue;
        }
        VkRect2D scissor{};
        scissor.offset = {x0, y0};
        scissor.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        const VkPipeline pipeline = pipelines[static_cast<size_t>(pipeline_kind)]->getHandle();
        if (pipeline != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            pipelines[static_cast<size_t>(pipeline_kind)]->useResource(timeline_semaphore, signal_value);
            bound_pipeline = pipeline;
        }
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_reference);

        RenderUITexture& texture = command.texture ? *command.texture : white_texture;
        const VkDescriptorSet set = texture.prepare(device);
        if (set != bound_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, nullptr);
            bound_set = set;
        }
        texture.useResource(timeline_semaphore, signal_value);

        UIPushConstants constants{};
        constants.transform = projection;
        if (command.transform >= 0 && static_cast<size_t>(command.transform) < transforms.size()) {
            constants.transform = projection * transforms[static_cast<size_t>(command.transform)];
        }
        constants.translation = command.translation;
        if (command.kind == UIDrawKind::GRADIENT) {
            constants.flags = UI_FLAG_GRADIENT;
            constants.gradient_function = command.gradient.function;
            constants.gradient_p = command.gradient.p;
            constants.gradient_v = command.gradient.v;
            constants.first_stop = command.gradient.first_stop;
            constants.last_stop = command.gradient.last_stop;
            constants.ramp_start = command.gradient.ramp_start;
            constants.ramp_end = command.gradient.ramp_end;
        }
        else if (command.texture) {
            constants.flags = UI_FLAG_TEXTURE;
        }
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);

        vkCmdDrawIndexed(cmd, command.index_count, 1, command.first_index, command.vertex_offset, 0);
    }
}

} // namespace gc
