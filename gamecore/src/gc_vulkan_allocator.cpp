#include "gamecore/gc_vulkan_allocator.h"

#include <cstdio> // snprintf for vma

#include <Volk/volk.h>

#include <vma/vk_mem_alloc.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_logger.h"

namespace gc {

std::optional<VmaAllocator> vulkanAllocatorCreate(VkInstance instance, const VulkanDevice& device)
{
    VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = nullptr,
        .vkGetDeviceProcAddr = nullptr,
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR = vkBindBufferMemory2,
        .vkBindImageMemory2KHR = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
        .vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements,
        .vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements,
    };

    VmaAllocatorCreateInfo createInfo{.flags = 0,
                                      .physicalDevice = device.physical_device,
                                      .device = device.device,
                                      .preferredLargeHeapBlockSize = 0, // set to zero for default, which is currently 256 MiB
                                      .pAllocationCallbacks = nullptr,
                                      .pDeviceMemoryCallbacks = nullptr,
                                      .pHeapSizeLimit = nullptr,
                                      .pVulkanFunctions = &functions,
                                      .instance = instance,
                                      .vulkanApiVersion = VK_API_VERSION_1_3,
                                      .pTypeExternalMemoryHandleTypes = nullptr};

    if (std::find(device.extensions_enabled.begin(), device.extensions_enabled.end(), std::string(VK_KHR_MAINTENANCE_4_EXTENSION_NAME)) !=
        device.extensions_enabled.end()) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (std::find(device.extensions_enabled.begin(), device.extensions_enabled.end(), std::string(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME)) !=
        device.extensions_enabled.end()) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }
    if (std::find(device.extensions_enabled.begin(), device.extensions_enabled.end(), std::string(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) !=
        device.extensions_enabled.end()) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    createInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT; // promoted to Vulkan 1.3

    VmaAllocator allocator;
    if (VkResult res = vmaCreateAllocator(&createInfo, &allocator); res != VK_SUCCESS) {
        GC_ERROR("vmaCreateAllocator() error: {}", vulkanResToString(res));
        return {};
    }

    return allocator;
}

void vulkanAllocatorDestroy(VmaAllocator allocator) { vmaDestroyAllocator(allocator); }

} // namespace gc
