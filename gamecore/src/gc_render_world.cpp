#include "gamecore/gc_render_world.h"
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_format_specialisations.h"
#include "gamecore/gc_render_buffer.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_shadow_map.h"

namespace gc {

void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, std::span<const std::unique_ptr<GPUPipeline>> main_pipelines,
                                  std::span<const std::unique_ptr<GPUPipeline>> instancing_pipelines, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data, GPUDescriptorSet& frame_uniform_buffer_set,
                                  RenderShadowMap& shadow_map, RenderBuffer& instance_transforms_buffer)
{
    GC_ASSERT(cmd);
    GC_ASSERT(pipeline_layout);
    GC_ASSERT(main_pipelines.size() == MATERIAL_PIPELINE_VARIANTS);
    GC_ASSERT(instancing_pipelines.size() == MATERIAL_PIPELINE_VARIANTS);
    GC_ASSERT(timeline_semaphore);

    // Every pipeline has the same layout, so this descriptor set, and the material's, stay bound when the pipeline changes.
    frame_uniform_buffer_set.useResource(timeline_semaphore, signal_value);
    {
        const auto ds = frame_uniform_buffer_set.getHandle();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &ds, 0, nullptr);
    }
    shadow_map.bind(cmd, pipeline_layout, timeline_semaphore, signal_value);

    GPUPipeline* last_bound_pipeline = nullptr;
    RenderMaterial* last_bound_material = nullptr;
    RenderMesh* last_bound_mesh = nullptr;

    // binds the variant of the pipeline that suits the material
    const auto bind_pipeline = [&](std::span<const std::unique_ptr<GPUPipeline>> pipelines, const RenderMaterial& material) {
        GPUPipeline* const pipeline = pipelines[material.getPipelineVariant()].get();
        GC_ASSERT(pipeline);
        if (last_bound_pipeline != pipeline) {
            pipeline->useResource(timeline_semaphore, signal_value);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getHandle());
            last_bound_pipeline = pipeline;
        }
    };

    const auto draw_entry = [&](const WorldDrawEntry& entry) {
        GC_ASSERT(entry.mesh);
        GC_ASSERT(entry.material);

        if (entry.mesh->isUploaded() && entry.material->isUploaded()) {
            bind_pipeline(main_pipelines, *entry.material);

            if (last_bound_material != entry.material) {
                entry.material->bind(cmd, pipeline_layout, timeline_semaphore, signal_value);
                last_bound_material = entry.material;
            }

            if (last_bound_mesh != entry.mesh) {
                entry.mesh->bind(cmd, timeline_semaphore, signal_value);
                last_bound_mesh = entry.mesh;
            }

            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &entry.world_matrix);
            vkCmdDrawIndexed(cmd, entry.mesh->getNumIndices(), 1, 0, 0, 0);
        }
    };

    // Opaque and alpha tested things can be drawn in any order, as the depth buffer sorts them out. Alpha blended things are
    // drawn after all of those, so that they are mixed with what is behind them. They are found here and drawn at the end.
    std::vector<std::pair<float, const WorldDrawEntry*>> blended_entries{};

    // render non-instanced draws
    for (const auto& entry : draw_data.getDrawEntries()) {
        if (entry.material->getBlendMode() == MaterialBlendMode::ALPHA_BLEND) {
            // how far in front of the camera it is. The camera looks down -Z, so further away is more negative
            const float view_z = (draw_data.getViewMatrix() * entry.world_matrix[3]).z;
            blended_entries.emplace_back(view_z, &entry);
        }
        else {
            draw_entry(entry);
        }
    }

    if (!draw_data.getInstancedDrawEntries().empty()) {
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
                bind_pipeline(instancing_pipelines, *entry.material);

                if (last_bound_material != entry.material) {
                    entry.material->bind(cmd, pipeline_layout, timeline_semaphore, signal_value);
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

    // Alpha blended draws, furthest first, so that nearer ones are mixed over further ones. (They don't write depth, so the
    // order is all that puts one in front of another.) The RenderSystem doesn't make instanced draws of blended materials.
    std::sort(blended_entries.begin(), blended_entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& blended : blended_entries) {
        draw_entry(*blended.second);
    }
}

} // namespace gc
