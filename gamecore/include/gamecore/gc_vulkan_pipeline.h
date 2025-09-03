#pragma once
#if 0

#include <tuple>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

std::pair<VkPipeline, VkPipelineLayout> createPipeline(VkDescriptorSetLayout set_layout);
void destroyPipeline(VkPipeline pipeline, VkPipelineLayout layout);

} // namespace gc

#endif