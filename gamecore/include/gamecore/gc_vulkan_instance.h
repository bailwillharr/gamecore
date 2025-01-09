#pragma once

#include <volk.h>

namespace gc {

// This functions loads vulkan, verifies the version, and creates an instance.
// returns VK_NULL_HANDLE if Vulkan failed to initialise (Unsupported vulkan version)
// Ensure SDL window was already created with SDL_WINDOW_VULKAN flag
// Destroy instance with vkDestroyInstance()
VkInstance vulkanInitialise();

}
