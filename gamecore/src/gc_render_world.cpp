#include "gamecore/gc_render_world.h"
#include <vulkan/vulkan_core.h>

#include <glm/mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_format_specialisations.h"
#include "gamecore/gc_render_buffer.h"
#include "gamecore/gc_gpu_resources.h"

namespace gc {

void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout main_pipeline_layout, GPUPipeline& main_pipeline,
                                  VkPipelineLayout instancing_pipeline_layout, GPUPipeline& instancing_pipeline, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data, GPUDescriptorSet& frame_uniform_buffer_set,
                                  RenderBuffer& instance_transforms_buffer)
{
    GC_ASSERT(cmd);
    GC_ASSERT(main_pipeline_layout);
    GC_ASSERT(instancing_pipeline_layout);
    GC_ASSERT(timeline_semaphore);

    main_pipeline.useResource(timeline_semaphore, signal_value);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, main_pipeline.getHandle());

    // this descriptor set is used by both the main_pipeline and instancing_pipeline
    frame_uniform_buffer_set.useResource(timeline_semaphore, signal_value);
    {
        const auto ds = frame_uniform_buffer_set.getHandle();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, main_pipeline_layout, 0, 1, &ds, 0, nullptr);
    }

    RenderMaterial* last_bound_material = nullptr;
    RenderMesh* last_bound_mesh = nullptr;

    // render non-instanced draws
    for (const auto& entry : draw_data.getDrawEntries()) {
        GC_ASSERT(entry.mesh);
        GC_ASSERT(entry.material);

        if (entry.mesh->isUploaded() && entry.material->isUploaded()) {
            if (last_bound_material != entry.material) {
                entry.material->bind(cmd, main_pipeline_layout, timeline_semaphore, signal_value);
                last_bound_material = entry.material;
            }

            if (last_bound_mesh != entry.mesh) {
                entry.mesh->bind(cmd, timeline_semaphore, signal_value);
                last_bound_mesh = entry.mesh;
            }

            vkCmdPushConstants(cmd, main_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &entry.world_matrix);
            vkCmdDrawIndexed(cmd, entry.mesh->getNumIndices(), 1, 0, 0, 0);
        }
    }

    if (!draw_data.getInstancedDrawEntries().empty()) {

        instancing_pipeline.useResource(timeline_semaphore, signal_value);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instancing_pipeline.getHandle());

        // frame_uniform_buffer_set will still be bound

        {
            VkDeviceSize offset{0};
            VkBuffer buffer = instance_transforms_buffer.getBuffer();
            vkCmdBindVertexBuffers(cmd, 1, 1, &buffer, &offset);
        }

        // render instanced draws
        for (const auto& entry : draw_data.getInstancedDrawEntries()) {
            GC_ASSERT(entry.mesh);
            GC_ASSERT(entry.material);

            if (entry.mesh->isUploaded() && entry.material->isUploaded()) {
                if (last_bound_material != entry.material) {
                    entry.material->bind(cmd, instancing_pipeline_layout, timeline_semaphore, signal_value);
                    last_bound_material = entry.material;
                }

                if (last_bound_mesh != entry.mesh) {
                    entry.mesh->bind(cmd, timeline_semaphore, signal_value);
                    last_bound_mesh = entry.mesh;
                }

                vkCmdDrawIndexed(cmd, entry.mesh->getNumIndices(), entry.instance_count, 0, 0, entry.transform_offset);
            }
        }
    }
}

} // namespace gc
