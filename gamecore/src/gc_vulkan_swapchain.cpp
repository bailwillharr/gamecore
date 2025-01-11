#include "gamecore/gc_vulkan_swapchain.h"

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"

namespace gc {

VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, SDL_Window* window) : m_device(device)
{
    if (!SDL_Vulkan_GetPresentationSupport(device.getInstance(), device.getPhysicalDevice(), 0)) {
        abortGame("No Vulkan presentation support on queue family 0");
    }
    if (!SDL_Vulkan_CreateSurface(window, device.getInstance(), nullptr, &m_surface)) {
        abortGame("SDL_Vulkan_CreateSurface() error: {}", SDL_GetError());
    }
}

VulkanSwapchain::~VulkanSwapchain() { SDL_Vulkan_DestroySurface(m_device.getInstance(), m_surface, nullptr); }

} // namespace gc