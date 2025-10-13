#pragma once

#include <array>
#include <memory>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"

namespace gc {

class RenderMaterial {
    const std::shared_ptr<RenderTexture> m_base_color_texture{};
    const std::shared_ptr<RenderTexture> m_occlusion_roughness_metallic_texture{};
    const std::shared_ptr<RenderTexture> m_normal_texture{};
    const std::shared_ptr<GPUPipeline> m_pipeline{};
    VkDescriptorSet m_descriptor_set{};

public:
    RenderMaterial(VkDevice device, VkDescriptorPool descriptor_pool, VkDescriptorSetLayout descriptor_set_layout,
                   const std::shared_ptr<RenderTexture>& base_color_texture, const std::shared_ptr<RenderTexture>& occlusion_roughness_metallic_texture,
                   const std::shared_ptr<RenderTexture>& normal_texture, const std::shared_ptr<GPUPipeline>& pipeline)
        : m_base_color_texture(base_color_texture),
          m_occlusion_roughness_metallic_texture(occlusion_roughness_metallic_texture),
          m_normal_texture(normal_texture),
          m_pipeline(pipeline)
    {
        GC_ASSERT(device);
        GC_ASSERT(descriptor_pool);
        GC_ASSERT(descriptor_set_layout);
        GC_ASSERT(m_base_color_texture);
        GC_ASSERT(m_occlusion_roughness_metallic_texture);
        GC_ASSERT(m_normal_texture);
        GC_ASSERT(m_pipeline);

        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = descriptor_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &descriptor_set_layout;
        GC_CHECKVK(vkAllocateDescriptorSets(device, &info, &m_descriptor_set));

        std::array<VkDescriptorImageInfo, 3> descriptor_image_infos{};
        std::array<VkWriteDescriptorSet, 3> writes{};

        descriptor_image_infos[0].imageView = m_base_color_texture->getImageView().getHandle();
        descriptor_image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &descriptor_image_infos[0];

        descriptor_image_infos[1].imageView = m_occlusion_roughness_metallic_texture->getImageView().getHandle();
        descriptor_image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &descriptor_image_infos[1];

        descriptor_image_infos[2].imageView = m_normal_texture->getImageView().getHandle();
        descriptor_image_infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_descriptor_set;
        writes[2].dstBinding = 2;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &descriptor_image_infos[2];

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Binds pipeline and descriptor sets. Check isUploaded() first
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value,
              const RenderMaterial* last_used_material = nullptr) const
    {
        GC_ASSERT(cmd);
        GC_ASSERT(pipeline_layout);
        GC_ASSERT(timeline_semaphore);
        GC_ASSERT(m_pipeline->getHandle());

        if (!last_used_material || (last_used_material && last_used_material->m_pipeline != m_pipeline)) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->getHandle());
            m_pipeline->useResource(timeline_semaphore, signal_value);
        }

        if (!last_used_material || (last_used_material && last_used_material->m_descriptor_set != m_descriptor_set)) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &m_descriptor_set, 0, nullptr);
            m_base_color_texture->getImageView().useResource(timeline_semaphore, signal_value);
            m_occlusion_roughness_metallic_texture->getImageView().useResource(timeline_semaphore, signal_value);
            m_normal_texture->getImageView().useResource(timeline_semaphore, signal_value);
        }
    }

    // Checks that all textures for this material are uploaded
    bool isUploaded() const
    {
        GC_ASSERT(m_base_color_texture);
        GC_ASSERT(m_occlusion_roughness_metallic_texture);
        GC_ASSERT(m_normal_texture);
        return m_base_color_texture->isUploaded() && m_occlusion_roughness_metallic_texture->isUploaded() && m_normal_texture->isUploaded();
    }

    void waitForUpload() const
    {
        GC_ASSERT(m_base_color_texture);
        GC_ASSERT(m_occlusion_roughness_metallic_texture);
        GC_ASSERT(m_normal_texture);
        m_base_color_texture->waitForUpload();
        m_occlusion_roughness_metallic_texture->waitForUpload();
        m_normal_texture->waitForUpload();
    }
};

} // namespace gc