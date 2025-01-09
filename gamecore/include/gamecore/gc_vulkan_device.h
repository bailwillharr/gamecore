#pragma once

#include <vector>
#include <string>
#include <optional>
#include <string_view>

#include <volk.h>

namespace gc {

struct VulkanDeviceProperties {
    VkPhysicalDeviceProperties2 props{};
    inline VulkanDeviceProperties()
    {
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = nullptr;
    }
};

struct VulkanDeviceFeatures {
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{};
    VkPhysicalDeviceFeatures2 features{};

    inline VulkanDeviceFeatures()
    {
        dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dynamic_rendering.pNext = nullptr;
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &dynamic_rendering;
    }
};

class VulkanDevice {
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VulkanDeviceProperties m_properties{};
    VulkanDeviceFeatures m_features_enabled{};
    std::vector<std::string> m_extensions_enabled{};

public:
    VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;

    ~VulkanDevice();

    VulkanDevice operator=(const VulkanDevice&) = delete;

    inline VkInstance getInstance() const { return m_instance; }
    inline VkDevice getDevice() const { return m_device; }
    inline VkPhysicalDevice getPhysicalDevice() const { return m_physical_device; }

    bool isExtensionEnabled(std::string_view name) const;
};

} // namespace gc
