#pragma once

#include <optional>

#include <Volk/volk.h>

#include <vma/vk_mem_alloc.h>

#include "gamecore/gc_vulkan_device.h"

namespace gc {

std::optional<VmaAllocator> vulkanAllocatorCreate(VkInstance instance, const VulkanDevice& device);
void vulkanAllocatorDestroy(VmaAllocator allocator);

}