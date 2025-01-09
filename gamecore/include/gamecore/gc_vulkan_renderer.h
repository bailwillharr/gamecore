#pragma once

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"

namespace gc {

class VulkanRenderer {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;

public:
    VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;

    ~VulkanRenderer();

    VulkanRenderer operator=(const VulkanRenderer&) = delete;
};

} // namespace gc