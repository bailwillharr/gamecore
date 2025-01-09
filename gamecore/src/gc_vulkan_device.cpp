/* Create the Vulkan graphics device and load its Vulkan function pointers. */
/* No optional extension/feature checking will be done, as that heavily complicates code. */
/* Any extension/feature used in the engine is required. */
/* Debug message callback and validation layers are enabled if GC_VULKAN_DEBUG is defined. */

#include "gamecore/gc_vulkan_device.h"

#include <array>
#include <algorithm>
#include <optional>
#include <vector>
#include <tuple>

#include <SDL3/SDL_vulkan.h>

#include <volk.h>

#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

static VkBool32 vulkanMessageCallback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity, VkDebugUtilsMessageTypeFlagsEXT message_types,
                                      const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data)
{
    std::string message_type{"("};
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        message_type += "VERBOSE ";
    }
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        message_type += "INFO ";
    }
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        message_type += "WARNING ";
    }
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        message_type += "WARNING ";
    }
    if (message_type.size() > 1) {
        message_type.back() = ')';
    }
    else {
        message_type.clear();
    }

    GC_DEBUG("Vulkan debug callback said: {} {}", message_type, callback_data->pMessage);
    return VK_FALSE;
}

static std::vector<VkQueueFamilyProperties> getQueueFamilyProperties(VkPhysicalDevice physical_device)
{
    uint32_t queue_family_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_family_properties.data());
    return queue_family_properties;
}

static VkPhysicalDevice choosePhysicalDevice(VkInstance instance)
{
    GC_ASSERT(instance != VK_NULL_HANDLE);

    uint32_t physical_device_count{};
    if (VkResult res = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr); res != VK_SUCCESS) {
        GC_ERROR("vkEnumeratePhysicalDevices() error: {}", vulkanResToString(res));
        return VK_NULL_HANDLE;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (VkResult res = vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()); res != VK_SUCCESS) {
        GC_ERROR("vkEnumeratePhysicalDevices() error: {}", vulkanResToString(res));
        return VK_NULL_HANDLE;
    }

    if (physical_devices.empty()) {
        GC_ERROR("No Vulkan physical device found.");
        return VK_NULL_HANDLE;
    }

    return physical_devices[0];
}

static std::vector<const char*> getRequiredExtensionNames()
{
    std::vector<const char*> exts{};

    // none yet

    return exts;
}

VulkanDevice::VulkanDevice()
{

    { // initialise volk with getInstanceProcAddr from SDL
        const auto my_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!my_vkGetInstanceProcAddr) {
            abortGame("SDL_Vulkan_GetVkGetInstanceProcAddr() error: {}", SDL_GetError());
        }
        volkInitializeCustom(my_vkGetInstanceProcAddr);
    }

    { // check version
        const uint32_t instance_version = volkGetInstanceVersion();
        if (VK_API_VERSION_VARIANT(instance_version) != VK_API_VERSION_VARIANT(REQUIRED_VULKAN_VERSION) ||
            VK_API_VERSION_MAJOR(instance_version) != VK_API_VERSION_MAJOR(REQUIRED_VULKAN_VERSION) ||
            VK_API_VERSION_MINOR(instance_version) < VK_API_VERSION_MINOR(REQUIRED_VULKAN_VERSION)) {
            abortGame("System Vulkan version is unsupported! Found: {}, Required: {}", vulkanVersionToString(instance_version),
                      vulkanVersionToString(REQUIRED_VULKAN_VERSION));
        }
    }

    { // create instance
        const std::vector<const char*> instance_extensions{};
        const std::vector<const char*> instance_layers{};

        VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info{};
        debug_messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_messenger_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_messenger_info.pfnUserCallback = vulkanMessageCallback;
        debug_messenger_info.pUserData = nullptr;

        const char* const ext_debug_utils_extension_name = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const char* const khronos_validation_layer_name = "VK_LAYER_KHRONOS_validation";

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
#ifdef GC_VULKAN_DEBUG
        instance_info.pNext = &debug_messenger_info;
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &khronos_validation_layer_name;
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = &ext_debug_utils_extension_name;
#endif

        if (VkResult res = vkCreateInstance(&instance_info, nullptr, &m_instance); res != VK_SUCCESS) {
            abortGame("vkCreateInstance() error: {}", vulkanResToString(res));
        }

        volkLoadInstanceOnly(m_instance);

#ifdef GC_VULKAN_DEBUG
        vkCreateDebugUtilsMessengerEXT(m_instance, &debug_messenger_info, nullptr, &m_debug_messenger);
#endif
    }

    { // Get suitable physical device
        m_physical_device = choosePhysicalDevice(m_instance);
        if (!m_physical_device) {
            if (m_debug_messenger) {
                vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
            }
            vkDestroyInstance(m_instance, nullptr);
            abortGame("Failed to find a Vulkan physical device");
        }
    }

    { // get physical device properties
        vkGetPhysicalDeviceProperties2(m_physical_device, &m_properties.props);
    }

    GC_DEBUG("Using Vulkan physical device: {}", m_properties.props.properties.deviceName);

    {
        auto queue_family_properties = getQueueFamilyProperties(m_physical_device);
        {
            // print debug info about available queues
            for (const VkQueueFamilyProperties& props : queue_family_properties) {
                props.
            }
        }
        std::vector<VkDeviceQueueCreateInfo> queue_infos{};

        const std::vector<const char*> extensions_to_enable = getRequiredExtensionNames();

        // enable features here:
        m_features_enabled.dynamic_rendering.dynamicRendering = VK_FALSE;

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &m_features_enabled.features;
        device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<uint32_t>(extensions_to_enable.size());
        device_info.ppEnabledExtensionNames = extensions_to_enable.data();
        device_info.pEnabledFeatures = nullptr; // using VkPhysicalDeviceFeatures2
        if (VkResult res = vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device); res != VK_SUCCESS) {
            if (m_debug_messenger) {
                vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
            }
            vkDestroyInstance(m_instance, nullptr);
            abortGame("vkCreateDevice() error: {}", vulkanResToString(res));
        }

        // copy extension strings
        for (const char* const ext_c : extensions_to_enable) {
            m_extensions_enabled.push_back(ext_c); // copy to std::string
        }
    }

    volkLoadDevice(m_device);

    GC_TRACE("Initialised VulkanDevice");
}

VulkanDevice::~VulkanDevice()
{
    GC_TRACE("Destroying VulkanDevice...");
    vkDestroyDevice(m_device, nullptr);
    if (m_debug_messenger) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

bool VulkanDevice::isExtensionEnabled(std::string_view name) const
{
    return std::find(m_extensions_enabled.cbegin(), m_extensions_enabled.cend(), name) != m_extensions_enabled.cend();
}

} // namespace gc