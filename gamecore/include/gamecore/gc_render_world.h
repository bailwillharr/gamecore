#pragma once

#include <glm/mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

class WorldDrawData;    // forward-dec
class GPUPipeline;      // forward-dec
class GPUDescriptorSet; // forward-dec
class RenderBuffer; // forward-dec

// To be called in a render pass instance.
// Dynamic viewport and scissors states should have already been set.
void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout main_pipeline_layout, GPUPipeline& main_pipeline,
                                  VkPipelineLayout instancing_pipeline_layout, GPUPipeline& instancing_pipeline, VkSemaphore timeline_semaphore,
                                  uint64_t signal_value, const WorldDrawData& draw_data, GPUDescriptorSet& frame_uniform_buffer_set,
                                  RenderBuffer& instance_transforms_buffer);

} // namespace gc
