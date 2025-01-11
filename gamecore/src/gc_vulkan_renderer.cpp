#include "gamecore/gc_vulkan_renderer.h"

#include <SDL3/SDL_vulkan.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"

namespace gc {

VulkanRenderer::VulkanRenderer(SDL_Window* window_handle)
    : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle) { GC_TRACE("Initialised VulkanRenderer"); }

VulkanRenderer::~VulkanRenderer() { GC_TRACE("Destroying VulkanRenderer..."); }

} // namespace gc