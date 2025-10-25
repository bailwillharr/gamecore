#include "gamecore/gc_render_world.h"

#include <mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_render_backend.h"

namespace gc {

void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout world_pipeline_layout, VkSemaphore timeline_semaphore, uint64_t signal_value,
                                  const WorldDrawData& draw_data)
{

    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, &draw_data.getViewMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 128, 64, &draw_data.getProjectionMatrix());
    vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 192, sizeof(glm::vec3), &draw_data.getLightPos());

    const RenderMaterial* last_material{};
    for (const auto& entry : draw_data.getDrawEntries()) {
        GC_ASSERT(entry.mesh);
        GC_ASSERT(entry.material);

        if (entry.mesh->isUploaded()) {
            if (entry.material->isUploaded()) {
                entry.material->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value, last_material);
                last_material = entry.material;
            }
            else {
                draw_data.getFallbackMaterial()->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value, last_material);
                last_material = draw_data.getFallbackMaterial();
            }
            vkCmdPushConstants(cmd, world_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &entry.world_matrix);
            entry.mesh->draw(cmd, timeline_semaphore, signal_value);
        }
    }

    if (RenderMaterial* skybox_material = draw_data.getSkyboxMaterial()) {
        if (skybox_material->isUploaded()) {
            skybox_material->bind(cmd, world_pipeline_layout, timeline_semaphore, signal_value, nullptr);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
    }
}

} // namespace gc
