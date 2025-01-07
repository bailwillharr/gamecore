#pragma once

#include <optional>

#include <Volk/volk.h>

namespace gc {

struct VulkanDevice {
    VkPhysicalDevice physical_device;
    VkDevice device;
};

std::optional<VulkanDevice> vulkanCreateDevice(VkInstance instance);

} // namespace gc