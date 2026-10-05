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

// To be called in a render pass instance.
// Dynamic viewport and scissors states should have already been set.
// The pipelines are indexed by MaterialFeatures. Every material in draw_data must have a pipeline for its features.
// shadow_map is what gets bound, whatever draw_data says: the caller decides (it must be uploaded).
void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout, std::span<const std::unique_ptr<GPUPipeline>> main_pipelines,
                                  std::span<const std::unique_ptr<GPUPipeline>> instancing_pipelines, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data, GPUDescriptorSet& frame_uniform_buffer_set,
                                  RenderShadowMap& shadow_map, RenderBuffer& instance_transforms_buffer);

} // namespace gc
