/* Create the Vulkan graphics device and load its Vulkan function pointers. */
/* No optional extension/feature checking will be done, as that heavily complicates code. */
/* Any extension/feature used in the engine is required. */

#include "gamecore/gc_vulkan_device.h"

#include <optional>
#include <vector>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

static std::optional<VkPhysicalDevice> choosePhysicalDevice(VkInstance instance)
{
    GC_ASSERT(instance != VK_NULL_HANDLE);

    uint32_t physical_device_count{};
    if (VkResult res = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr); res != VK_SUCCESS) {
        GC_ERROR("vkEnumeratePhysicalDevices() error: {}", vulkanResToString(res));
        return {};
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (VkResult res = vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()); res != VK_SUCCESS) {
        GC_ERROR("vkEnumeratePhysicalDevices() error: {}", vulkanResToString(res));
        return {};
    }

    if (physical_devices.empty()) {
        GC_ERROR("No Vulkan physical device found.");
        return {};
    }

    return physical_devices[0];
}

static std::vector<const char*> getRequiredExtensionNames()
{
    std::vector<const char*> exts{};

    // none yet

    return exts;
}

std::optional<VulkanDevice> vulkanCreateDevice(VkInstance instance)
{
    GC_ASSERT(instance != VK_NULL_HANDLE);

    VulkanDevice device;

    {
        auto physical_device = choosePhysicalDevice(instance);
        if (!physical_device) {
            GC_ERROR("Failed to find a Vulkan physical device");
            return {};
        }
        device.physical_device = physical_device.value();
    }

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device.physical_device, &props);
        GC_INFO("Using Vulkan physical device: {}", props.deviceName);
    }

    {
        std::vector<VkDeviceQueueCreateInfo> queue_infos{};
        float queue_priority = 1.0f;
        queue_infos.push_back(VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                      .pNext = nullptr,
                                                      .flags = 0,
                                                      .queueFamilyIndex = 0,
                                                      .queueCount = 1,
                                                      .pQueuePriorities = &queue_priority});
        const std::vector<const char*> extensions_to_enable = getRequiredExtensionNames();

        VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{};
        dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &dynamic_rendering;

        // enable features here:
        // none yet

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &features2;
        device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<uint32_t>(extensions_to_enable.size());
        device_info.ppEnabledExtensionNames = extensions_to_enable.data();
        device_info.pEnabledFeatures = nullptr; // using VkPhysicalDeviceFeatures2
        if (VkResult res = vkCreateDevice(device.physical_device, &device_info, nullptr, &device.device); res != VK_SUCCESS) {
            GC_ERROR("vkCreateDevice() error: {}", vulkanResToString(res));
            return {};
        }

        // copy extension strings
        for (const char* const ext_c : extensions_to_enable) {
            device.extensions_enabled.push_back(ext_c); // copy to std::string
        }
    }

    volkLoadDevice(device.device);

    return device;
}

} // namespace gc