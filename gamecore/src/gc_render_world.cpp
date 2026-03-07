#include "gamecore/gc_render_world.h"
#include <vulkan/vulkan_core.h>

#include <glm/mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_format_specialisations.h"
#include "gamecore/gc_render_buffer.h"

namespace gc {

void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout world_pipeline_layout, GPUPipeline& world_pipeline, GPUPipeline& instancing_pipeline,
                                  VkSemaphore timeline_semaphore, uint64_t signal_value, const WorldDrawData& draw_data,
                                  VkBuffer instance_transforms_buffer)
{
    GC_ASSERT(cmd);
    GC_ASSERT(world_pipeline_layout);
    GC_ASSERT(timeline_semaphore);

    world_pipeline.useResource(timeline_semaphore, signal_value);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline.getHandle());

    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, &draw_data.getViewMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 128, 64, &draw_data.getProjectionMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 192, sizeof(glm::vec3), &draw_data.getLightPos());

    RenderMaterial* last_bound_material = nullptr;
    RenderMesh* last_bound_mesh = nullptr;

    // render non-instanced draws
    for (const auto& entry : draw_data.getDrawEntries()) {
        GC_ASSERT(entry.mesh);
        GC_ASSERT(entry.material);

        if (entry.mesh->isUploaded() && entry.material->isUploaded()) {
            if (last_bound_material != entry.material) {
                entry.material->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value);
                last_bound_material = entry.material;
            }

            if (last_bound_mesh != entry.mesh) {
                entry.mesh->bind(cmd, timeline_semaphore, signal_value);
                last_bound_mesh = entry.mesh;
            }

            vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &entry.world_matrix);
            vkCmdDrawIndexed(cmd, entry.mesh->getNumIndices(), 1, 0, 0, 0);
        }
    }

    if (!draw_data.getInstancedDrawEntries().empty()) {

        instancing_pipeline.useResource(timeline_semaphore, signal_value);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instancing_pipeline.getHandle());

        {
            VkDeviceSize offset{0};
            VkBuffer buffer = instance_transforms_buffer;
            vkCmdBindVertexBuffers(cmd, 1, 1, &buffer, &offset);
        }

        // render instanced draws
        for (const auto& entry : draw_data.getInstancedDrawEntries()) {
            GC_ASSERT(entry.mesh);
            GC_ASSERT(entry.material);

            if (entry.mesh->isUploaded() && entry.material->isUploaded()) {
                if (last_bound_material != entry.material) {
                    entry.material->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value);
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
