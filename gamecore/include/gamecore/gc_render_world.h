#pragma once

#include <mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

class WorldDrawData; // forward-dec

// To be called in a render pass instance.
// Dynamic viewport and scissors states should have already been set.
void recordWorldRenderingCommands(VkCommandBuffer cmd, VkPipelineLayout world_pipeline_layout,
                                  VkSemaphore timeline_semaphore, uint64_t signal_value, const WorldDrawData& draw_data);

} // namespace gc