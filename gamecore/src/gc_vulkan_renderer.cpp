#include "gamecore/gc_vulkan_renderer.h"

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_logger.h"

namespace gc {

VulkanRenderer::VulkanRenderer()
    : m_device(), m_allocator(m_device) { GC_TRACE("Initialised VulkanRenderer"); }

VulkanRenderer::~VulkanRenderer() { GC_TRACE("Destroying VulkanRenderer..."); }

} // namespace gc