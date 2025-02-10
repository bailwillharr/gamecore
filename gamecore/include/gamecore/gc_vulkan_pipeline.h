#pragma once

#include <tuple>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

std::pair<VkPipeline, VkPipelineLayout> createPipeline();
void destroyPipeline(VkPipeline pipeline, VkPipelineLayout layout);

} // namespace gc