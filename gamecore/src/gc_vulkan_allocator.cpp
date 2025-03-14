#include "gamecore/gc_vulkan_allocator.h"

#include <cstdio> // snprintf for vma

#include <volk.h>

#include <vk_mem_alloc.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_abort.h"

namespace gc {

VulkanAllocator::VulkanAllocator(const VulkanDevice& device)
{
    VmaVulkanFunctions functions{.vkGetInstanceProcAddr = nullptr,
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
                                 .vkGetMemoryWin32HandleKHR = nullptr};

    VmaAllocatorCreateInfo createInfo{.flags = 0,
                                      .physicalDevice = device.getPhysicalDevice(),
                                      .device = device.getHandle(),
                                      .preferredLargeHeapBlockSize = 0, // set to zero for default, which is currently 256 MiB
                                      .pAllocationCallbacks = nullptr,
                                      .pDeviceMemoryCallbacks = nullptr,
                                      .pHeapSizeLimit = nullptr,
                                      .pVulkanFunctions = &functions,
                                      .instance = device.getInstance(),
                                      .vulkanApiVersion = VK_API_VERSION_1_3,
                                      .pTypeExternalMemoryHandleTypes = nullptr};

    if (device.isExtensionEnabled(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME)) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }
    if (device.isExtensionEnabled(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    createInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT; // promoted to Vulkan 1.3

    if (VkResult res = vmaCreateAllocator(&createInfo, &m_handle); res != VK_SUCCESS) {
        abortGame("vmaCreateAllocator() error: {}", vulkanResToString(res));
    }

    GC_TRACE("Initialised VulkanAllocator");
}

VulkanAllocator::~VulkanAllocator()
{
    GC_TRACE("Destroying VulkanAllocator...");
    vmaDestroyAllocator(m_handle);
}

VmaAllocator VulkanAllocator::getHandle() const { return m_handle; }

} // namespace gc
