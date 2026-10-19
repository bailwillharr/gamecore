#pragma once

#include <memory>
#include <span>

#include <glm/mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

class WorldDrawData;    // forward-dec
class GPUPipeline;      // forward-dec
class GPUDescriptorSet; // forward-dec
class RenderBuffer; // forward-dec
class RenderShadowMap; // forward-dec
class RenderEnvironment; // forward-dec
class RenderTerrain;     // forward-dec

// To be called in a render pass instance.
// Dynamic viewport and scissors states should have already been set.
// The pipelines are indexed by MaterialFeatures. Every material in draw_data must have a pipeline for its features.
// shadow_map and environment are what get bound, whatever draw_data says: the caller decides (they must be uploaded).
// If sky_pipeline isn't null, the sky is drawn with it: after everything that is opaque, so that only the part of it that can be
// seen is shaded, and before what is blended, so that it is seen through that.
// Terrains are drawn after the other opaque things, with the terrain pipelines (indexed by MaterialFeatures), whose layout has to be
// compatible with pipeline_layout for sets 0 to 3. Their patches are in terrain_patches_buffer, as in draw_data.
void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, std::span<const std::unique_ptr<GPUPipeline>> main_pipelines,
                                  std::span<const std::unique_ptr<GPUPipeline>> instancing_pipelines, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data, GPUDescriptorSet& frame_uniform_buffer_set,
                                  RenderShadowMap& shadow_map, RenderEnvironment& environment, GPUPipeline* sky_pipeline,
                                  RenderBuffer& instance_transforms_buffer, VkPipelineLayout terrain_pipeline_layout,
                                  std::span<const std::unique_ptr<GPUPipeline>> terrain_pipelines, RenderBuffer& terrain_patches_buffer);

} // namespace gc
