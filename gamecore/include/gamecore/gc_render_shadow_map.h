#pragma once

#include "gamecore/gc_assert.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

// A shadow map texture and the descriptor set that the world's shaders read it through (set 2).
// Create with RenderBackend::createShadowMap()
class RenderShadowMap {
    RenderTexture m_texture;
    GPUDescriptorSet m_descriptor_set;

public:
    // takes exclusive ownership of the texture and the descriptor set
    RenderShadowMap(VkDevice device, RenderTexture&& texture, GPUDescriptorSet&& descriptor_set)
        : m_texture(std::move(texture)), m_descriptor_set(std::move(descriptor_set))
    {
        GC_ASSERT(device);

        VkDescriptorImageInfo image_info{};
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
    }

    RenderShadowMap(RenderShadowMap&&) = default;

    // Check isUploaded() first
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value)
    {
        GC_ASSERT(cmd);
        GC_ASSERT(pipeline_layout);
        GC_ASSERT(timeline_semaphore);

        m_descriptor_set.useResource(timeline_semaphore, signal_value);
        m_texture.useResource(timeline_semaphore, signal_value);

        const VkDescriptorSet handle = m_descriptor_set.getHandle();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 2, 1, &handle, 0, nullptr);
    }

    bool isUploaded() const { return m_texture.isUploaded(); }

    void waitForUpload() const { m_texture.waitForUpload(); }
};

} // namespace gc
