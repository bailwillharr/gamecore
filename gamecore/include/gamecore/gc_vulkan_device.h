#pragma once

#include <vector>
#include <string>
#include <optional>

#include <Volk/volk.h>

namespace gc {

struct VulkanDevice {
    VkPhysicalDevice physical_device;
    VkDevice device;
    std::vector<std::string> extensions_enabled;
    VkQueue queue;
};

// Call vkDestroyDevice(device.device) at shutdown time
std::optional<VulkanDevice> vulkanCreateDevice(VkInstance instance);

} // namespace gc