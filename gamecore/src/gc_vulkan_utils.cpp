#include "gamecore/gc_vulkan_utils.h"

namespace gc::vkutils {

std::pair<VkImage, VmaAllocation> createImage(VmaAllocator allocator, VkFormat format, uint32_t width, uint32_t height, uint32_t mip_levels,
                                              VkSampleCountFlagBits msaa_samples, VkImageUsageFlags usage, float priority, bool dedicated, bool cube_map)
{
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = nullptr;
    image_info.flags = cube_map ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = cube_map ? 6 : 1;
    image_info.samples = msaa_samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;     // ignored
    image_info.pQueueFamilyIndices = nullptr; // ingored
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = dedicated ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_create_info.preferredFlags = (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ? VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT : 0;
    alloc_create_info.priority = priority; 
    VkImage image{};
    VmaAllocation allocation{};
    GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &image, &allocation, nullptr));
    return std::make_pair(image, allocation);
}

VkImageView createImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels, bool cube_map)
{
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = nullptr;
    view_info.flags = 0;
    view_info.image = image;
    view_info.viewType = cube_map ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = cube_map ? 6 : 1;
    VkImageView view{};
    GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &view));
    return view;
}

} // namespace gc::vkutils