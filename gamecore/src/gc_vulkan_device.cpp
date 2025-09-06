/* Create the Vulkan graphics device and load its Vulkan function pointers. */
/* Debug message callback and validation layers are enabled if GC_VULKAN_VALIDATION is defined. */

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

static VkBool32 vulkanMessageCallback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity, [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT message_types,
                                      const VkDebugUtilsMessengerCallbackDataEXT* callback_data, [[maybe_unused]] void* user_data)
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

    GC_WARN("Vulkan debug callback said: {} {}", message_type, callback_data->pMessage);
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

static void removeUnsupportedExtensions(VkPhysicalDevice physical_device, std::vector<const char*>& exts)
{
    uint32_t count{};
    if (VkResult res = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr); res != VK_SUCCESS) {
        abortGame("vkEnumerateDeviceExtensionProperties() error: {}", vulkanResToString(res));
    }
    std::vector<VkExtensionProperties> extension_properties(count);
    if (VkResult res = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extension_properties.data()); res != VK_SUCCESS) {
        abortGame("vkEnumerateDeviceExtensionProperties() error: {}", vulkanResToString(res));
    }
    // not sure if string pointers are always the same so using strcmp just in case
    for (size_t i = 0; i < exts.size(); ++i) {
        bool supported = false;
        for (const VkExtensionProperties& found_extension : extension_properties) {
            if (strncmp(exts[i], found_extension.extensionName, sizeof(VkExtensionProperties::extensionName) - 1) == 0) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            GC_WARN("Vulkan extension unsupported: {}", exts[i]);
            exts.erase(exts.cbegin() + i);
            i = i - 1; // avoid skipping
        }
    }
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
        GC_DEBUG("Vulkan instance version: {}", vulkanVersionToString(instance_version));
    }

    { // create instance
        std::vector<const char*> instance_extensions{};

        /* get SDL window required extensions for swapchain */
        uint32_t window_extension_count{};
        const char* const* const window_extensions = SDL_Vulkan_GetInstanceExtensions(&window_extension_count);
        if (!window_extensions) {
            abortGame("SDL_Vulkan_GetInstanceExtensions() error: ", SDL_GetError());
        }
        for (uint32_t i = 0; i < window_extension_count; ++i) {
            instance_extensions.push_back(window_extensions[i]);
        }

        // The above code pushes back VK_KHR_surface and VK_KHR_win32_surface on windows

#ifdef GC_VULKAN_VALIDATION
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
        instance_extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        instance_extensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

        VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info{};
        debug_messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_messenger_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_messenger_info.pfnUserCallback = vulkanMessageCallback;
        debug_messenger_info.pUserData = nullptr;

        [[maybe_unused]] const char* const khronos_validation_layer_name = "VK_LAYER_KHRONOS_validation";

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
        instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
#ifdef GC_VULKAN_VALIDATION
        instance_info.pNext = &debug_messenger_info;
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &khronos_validation_layer_name;

        GC_DEBUG("Using Vulkan validation layers.");
#endif

        if (VkResult res = vkCreateInstance(&instance_info, nullptr, &m_instance); res != VK_SUCCESS) {
            abortGame("vkCreateInstance() error: {}", vulkanResToString(res));
        }

        volkLoadInstanceOnly(m_instance);

#ifdef GC_VULKAN_VALIDATION
        vkCreateDebugUtilsMessengerEXT(m_instance, &debug_messenger_info, nullptr, &m_debug_messenger);
#endif
    }

    m_physical_device = choosePhysicalDevice(m_instance);
    if (!m_physical_device) {
        if (m_debug_messenger) {
            vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
        }
        vkDestroyInstance(m_instance, nullptr);
        abortGame("Failed to find a Vulkan physical device");
    }

    vkGetPhysicalDeviceProperties2(m_physical_device, &m_properties.props);

    GC_DEBUG("Using Vulkan physical device: {}", m_properties.props.properties.deviceName);

    {
        auto queue_family_properties = getQueueFamilyProperties(m_physical_device);

        /* get primary queue family (first queue that supports graphics) */
        uint32_t main_queue_family_queue_count{};
        m_main_queue = VK_NULL_HANDLE; // this is set just after device is created
        m_main_queue_family_index = 0;
        for (const VkQueueFamilyProperties& props : queue_family_properties) {
            if (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                main_queue_family_queue_count = props.queueCount;
                break;
            }
            ++m_main_queue_family_index;
        }
        if (m_main_queue_family_index == queue_family_properties.size()) {
            abortGame("No Vulkan device queue with graphics support found.");
        }

        if (!SDL_Vulkan_GetPresentationSupport(m_instance, m_physical_device, m_main_queue_family_index)) {
            /* Ensure queue family #0 supports presentation. */
            /* All real Vulkan capable GPUs are going to support presentation. This is here just in case. */
            abortGame("Vulkan queue family #0 doesn't support presentation.");
        }

        // Separate present queue adds very slight delay (0.05ms) to frame time with no apparent benefit

        const std::vector<float> queue_priorities{1.0f};
        std::vector<VkDeviceQueueCreateInfo> queue_infos{};
        // Create a main queue for graphics operations
        queue_infos.push_back(VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                      .pNext = nullptr,
                                                      .queueFamilyIndex = m_main_queue_family_index,
                                                      .queueCount = 1u,
                                                      .pQueuePriorities = queue_priorities.data()});

        constexpr std::array<const char*, 2> REQUIRED_EXTENSION_NAMES{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        constexpr std::array<const char*, 2> OPTIONAL_EXTENSION_NAMES{VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};

        // First only add optional extension names and then remove any which are unsupported.
        // Then add required extensions without checking for compatibility and create the device.
        // If a required extension is unsupported, vkCreateDevice() will fail.
        std::vector<const char*> extensions_to_enable(OPTIONAL_EXTENSION_NAMES.cbegin(), OPTIONAL_EXTENSION_NAMES.cend());
        removeUnsupportedExtensions(m_physical_device, extensions_to_enable);
        extensions_to_enable.insert(extensions_to_enable.cend(), REQUIRED_EXTENSION_NAMES.cbegin(), REQUIRED_EXTENSION_NAMES.cend());

        // copy extension strings for isExtensionEnabled()
        for (const char* const ext_c : extensions_to_enable) {
            m_extensions_enabled.push_back(ext_c); // copy to std::string
        }

        // enable features here:
        m_features_enabled.swapchain_maintenance_1.swapchainMaintenance1 = VK_TRUE;
        m_features_enabled.vulkan13.dynamicRendering = VK_TRUE;
        m_features_enabled.vulkan13.synchronization2 = VK_TRUE;
        m_features_enabled.vulkan12.timelineSemaphore = VK_TRUE;
        if (isExtensionEnabled(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME)) {
            m_features_enabled.memory_priority.memoryPriority = VK_TRUE;
        }
        else {
            // unlink memory priority struct from m_features_enabled chain
            m_features_enabled.swapchain_maintenance_1.pNext = m_features_enabled.memory_priority.pNext;
        }
        m_features_enabled.features.features.samplerAnisotropy = VK_TRUE;

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

        volkLoadDevice(m_device);

        {
            // Get Queues
            vkGetDeviceQueue(m_device, m_main_queue_family_index, 0, &m_main_queue);
        }
    }

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
