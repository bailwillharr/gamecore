#pragma once

#include <array>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_render_texture.h"

namespace gc {

class RenderMaterial {
    RenderTexture& m_base_color_texture;
    RenderTexture& m_occlusion_roughness_metallic_texture;
    RenderTexture& m_normal_texture;

    GPUDescriptorSet m_descriptor_set;

    uint64_t m_last_used_frame = 0;

public:
    // takes exclusive ownership of the descriptor set (will free it)
    RenderMaterial(VkDevice device, GPUDescriptorSet&& descriptor_set, RenderTexture& base_color_texture, RenderTexture& occlusion_roughness_metallic_texture,
                   RenderTexture& normal_texture)
        : m_base_color_texture(base_color_texture),
          m_occlusion_roughness_metallic_texture(occlusion_roughness_metallic_texture),
          m_normal_texture(normal_texture),
          m_descriptor_set(std::move(descriptor_set))
    {
        GC_ASSERT(device);

        std::array<VkDescriptorImageInfo, 3> descriptor_image_infos{};
        std::array<VkWriteDescriptorSet, 3> writes{};

        descriptor_image_infos[0].imageView = m_base_color_texture.getImageView();
        descriptor_image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptor_set.getHandle();
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &descriptor_image_infos[0];

        descriptor_image_infos[1].imageView = m_occlusion_roughness_metallic_texture.getImageView();
        descriptor_image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptor_set.getHandle();
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &descriptor_image_infos[1];

        descriptor_image_infos[2].imageView = m_normal_texture.getImageView();
        descriptor_image_infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_descriptor_set.getHandle();
        writes[2].dstBinding = 2;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &descriptor_image_infos[2];

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        GC_TRACE("Created RenderMaterial");
    }

    RenderMaterial(RenderMaterial&&) = default;

    ~RenderMaterial() { GC_TRACE("Destroying RenderMaterial..."); }

    // Binds descriptor sets. Check isUploaded() first
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value) const
    {
        GC_ASSERT(cmd);
        GC_ASSERT(pipeline_layout);
        GC_ASSERT(timeline_semaphore);

        const VkDescriptorSet handle = m_descriptor_set.getHandle();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &handle, 0, nullptr);
        m_base_color_texture.useResource(timeline_semaphore, signal_value);
        m_occlusion_roughness_metallic_texture.useResource(timeline_semaphore, signal_value);
        m_normal_texture.useResource(timeline_semaphore, signal_value);
    }

    // Checks that all textures for this material are uploaded
    bool isUploaded() const
    {
        if (!m_base_color_texture.isUploaded()) {
            return false;
        }
        if (!m_occlusion_roughness_metallic_texture.isUploaded()) {
            return false;
        }
        if (!m_normal_texture.isUploaded()) {
            return false;
        }
        return true;
    }

    void waitForUpload() const
    {
        m_base_color_texture.waitForUpload();
        m_occlusion_roughness_metallic_texture.waitForUpload();
        m_normal_texture.waitForUpload();
    }

    uint64_t getLastUsedFrame() const { return m_last_used_frame; }
    void setLastUsedFrame(uint64_t last_used_frame)
    {
        GC_ASSERT(last_used_frame >= m_last_used_frame);
        m_last_used_frame = last_used_frame;
    }

    const auto& getBaseColorTexture() const { return m_base_color_texture; }
    const auto& getORMTexture() const { return m_occlusion_roughness_metallic_texture; }
    const auto& getNormalTexture() const { return m_normal_texture; }
};

} // namespace gc
