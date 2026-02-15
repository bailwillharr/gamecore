#include "gamecore/gc_render_world.h"
#include <vulkan/vulkan_core.h>

#include <mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_format_specialisations.h"

namespace gc {

void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout world_pipeline_layout, GPUPipeline& world_pipeline, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data)
{
    GC_ASSERT(cmd);
    GC_ASSERT(world_pipeline_layout);
    GC_ASSERT(timeline_semaphore);

    world_pipeline.useResource(timeline_semaphore, signal_value);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline.getHandle());

    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, &draw_data.getViewMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 128, 64, &draw_data.getProjectionMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 192, sizeof(glm::vec3), &draw_data.getLightPos());

    for (const auto& entry : draw_data.getDrawEntries()) {
        GC_ASSERT(entry.mesh);
        GC_ASSERT(entry.material);

        if (entry.mesh->isUploaded()) {
            if (entry.material->isUploaded()) {
                entry.material->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value);
            }
            else {
                auto fallback = draw_data.getFallbackMaterial();
                if (fallback) {
                    fallback->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value);
                }
                else {
                    GC_WARN("Material texture's not uploaded yet, but fallback material hasn't been set!");
                    continue;
                }
            }

            vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &entry.world_matrix);
            entry.mesh->draw(cmd, timeline_semaphore, signal_value);
        }
    }
}

} // namespace gc
