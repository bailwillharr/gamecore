#include "gamecore/gc_vulkan_device.h"

#include <optional>
#include <vector>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

std::optional<VulkanDevice> vulkanCreateDevice(VkInstance instance)
{
    GC_ASSERT(instance != VK_NULL_HANDLE);

    VulkanDevice device;
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

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_devices[0], &props);
    GC_INFO("Using Vulkan physical device: {}", props.deviceName);
    
    return device;
}

} // namespace gc