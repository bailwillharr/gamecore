#pragma once

#include <vector>
#include <string>
#include <optional>
#include <string_view>

#include "gc_vulkan_common.h"

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
    VkPhysicalDeviceVulkan13Features vulkan13{};
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceVulkan11Features vulkan11{};
    VkPhysicalDeviceFeatures2 features{};

    inline VulkanDeviceFeatures()
    {
        vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13.pNext = nullptr;
        vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12.pNext = &vulkan13;
        vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11.pNext = &vulkan12;
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &vulkan11;
    }
};

struct VulkanQueue {
    VkQueue queue;
    uint32_t queue_family_index;
};

class VulkanDevice {
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VulkanQueue m_main_queue{};
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
