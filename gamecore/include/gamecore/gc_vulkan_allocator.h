#pragma once

#include <optional>

#include <volk.h>

#include <vk_mem_alloc.h>

#include "gamecore/gc_vulkan_device.h"

namespace gc {

class VulkanAllocator {
    VmaAllocator m_handle{};

public:
    VulkanAllocator(const VulkanDevice& device);
    ~VulkanAllocator();
};

}
