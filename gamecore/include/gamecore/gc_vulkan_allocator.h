#pragma once

#include <optional>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"

namespace gc {

class VulkanAllocator {
    VmaAllocator m_handle{};

public:
    VulkanAllocator(const VulkanDevice& device);
    ~VulkanAllocator();
};

}
