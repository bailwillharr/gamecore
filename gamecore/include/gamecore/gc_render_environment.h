#pragma once

#include <array>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

// The cube maps of a sky (see SkyboxComponent) and the descriptor set that the shaders read them through (set 3):
// binding 0 is the sky as it is seen, binding 1 its irradiance and binding 2 its reflections, blurrier in each mip level.
// Create with RenderBackend::createEnvironment()
class RenderEnvironment {
    VkDevice m_device;
    RenderTexture m_sky;
    RenderTexture m_irradiance;
    RenderTexture m_specular;
    uint32_t m_specular_level_count;
    GPUDescriptorSet m_descriptor_set;
    // The descriptor set refers to the images of the cube maps, which don't exist until they have been uploaded
    mutable bool m_descriptor_set_written = false;

public:
    // takes exclusive ownership of the cube maps and the descriptor set
    RenderEnvironment(VkDevice device, RenderTexture&& sky, RenderTexture&& irradiance, RenderTexture&& specular, uint32_t specular_level_count,
                      GPUDescriptorSet&& descriptor_set)
        : m_device(device),
          m_sky(std::move(sky)),
          m_irradiance(std::move(irradiance)),
          m_specular(std::move(specular)),
          m_specular_level_count(specular_level_count),
          m_descriptor_set(std::move(descriptor_set))
    {
        GC_ASSERT(device);
        GC_ASSERT(specular_level_count >= 1);
    }

    RenderEnvironment(RenderEnvironment&&) = default;

    // Check isUploaded() first
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value)
    {
        GC_ASSERT(cmd);
        GC_ASSERT(pipeline_layout);
        GC_ASSERT(timeline_semaphore);
        GC_ASSERT(m_descriptor_set_written);

        m_descriptor_set.useResource(timeline_semaphore, signal_value);
        m_sky.useResource(timeline_semaphore, signal_value);
        m_irradiance.useResource(timeline_semaphore, signal_value);
        m_specular.useResource(timeline_semaphore, signal_value);

        const VkDescriptorSet handle = m_descriptor_set.getHandle();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 3, 1, &handle, 0, nullptr);
    }

    // Whether all three cube maps are uploaded. (They are small, apart from the sky, and are uploaded in the same order.)
    bool isUploaded() const
    {
        if (!m_sky.isUploaded() || !m_irradiance.isUploaded() || !m_specular.isUploaded()) {
            return false;
        }
        writeDescriptorSet();
        return true;
    }

    void waitForUpload() const
    {
        m_sky.waitForUpload();
        m_irradiance.waitForUpload();
        m_specular.waitForUpload();
        writeDescriptorSet();
    }

    // The reflections for the roughest surface are in the last level, and those for a mirror in the first
    uint32_t getSpecularLevelCount() const { return m_specular_level_count; }

private:
    // Points the descriptor set at the cube maps, the first time this is called. They must be uploaded
    void writeDescriptorSet() const
    {
        if (m_descriptor_set_written) {
            return;
        }
        m_descriptor_set_written = true;

        const std::array<VkImageView, 3> views{m_sky.getImageView(), m_irradiance.getImageView(), m_specular.getImageView()};
        std::array<VkDescriptorImageInfo, 3> image_infos{};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t i = 0; i < 3; ++i) {
            image_infos[i].imageView = views[i];
            image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_descriptor_set.getHandle();
            writes[i].dstBinding = i;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &image_infos[i];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
};

} // namespace gc
