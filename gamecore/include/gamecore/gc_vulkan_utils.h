#pragma once

#include <tuple>

#include "gamecore/gc_vulkan_common.h"

namespace gc::vkutils {

std::pair<VkImage, VmaAllocation> createImage(VmaAllocator allocator, VkFormat format, uint32_t width, uint32_t height, uint32_t mip_levels,
                                              VkSampleCountFlagBits msaa_samples, VkImageUsageFlags usage, float priority, bool dedicated = false, bool cube_map = false);

VkImageView createImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels, bool cube_map = false);

} // namespace gc::vkutils