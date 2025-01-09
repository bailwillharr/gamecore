#include "gamecore/gc_vulkan_instance.h"

#include <SDL3/SDL_vulkan.h>

#include <volk.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_logger.h"

namespace gc {

VkInstance vulkanInitialise()
{

    const auto my_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!my_vkGetInstanceProcAddr) {
        GC_ERROR("SDL_Vulkan_GetVkGetInstanceProcAddr() error: {}", SDL_GetError());
        return VK_NULL_HANDLE;
    }

    volkInitializeCustom(my_vkGetInstanceProcAddr);

    const uint32_t instance_version = volkGetInstanceVersion();
    if (VK_API_VERSION_VARIANT(instance_version) != VK_API_VERSION_VARIANT(REQUIRED_VULKAN_VERSION) ||
        VK_API_VERSION_MAJOR(instance_version) != VK_API_VERSION_MAJOR(REQUIRED_VULKAN_VERSION) ||
        VK_API_VERSION_MINOR(instance_version) < VK_API_VERSION_MINOR(REQUIRED_VULKAN_VERSION)) {
        GC_ERROR("System Vulkan version is unsupported! Found: {}, Required: {}", vulkanVersionToString(instance_version), vulkanVersionToString(REQUIRED_VULKAN_VERSION));
        return VK_NULL_HANDLE;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Gamecore Game";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.pEngineName = "Gamecore";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.apiVersion = REQUIRED_VULKAN_VERSION;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledLayerCount = 0;
    instance_info.ppEnabledLayerNames = nullptr;
    instance_info.enabledExtensionCount = 0;
    instance_info.ppEnabledExtensionNames = nullptr;

    VkInstance instance;
    if (VkResult res = vkCreateInstance(&instance_info, nullptr, &instance); res != VK_SUCCESS) {
        GC_ERROR("vkCreateInstance() error: {}", vulkanResToString(res));
        return VK_NULL_HANDLE;
    }

    volkLoadInstanceOnly(instance);

    return instance;
}

} // namespace gc
