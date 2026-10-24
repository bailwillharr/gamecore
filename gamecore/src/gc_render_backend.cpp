#include "gamecore/gc_render_backend.h"

#include <cstddef>
#include <cstring>

#include <algorithm>
#include <array>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_timer.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include "gamecore/gc_app.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gclog/gclog.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_ui_draw_data.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_render_world.h"
#include "gamecore/gc_resources.h"
#include "gamecore/gc_vulkan_utils.h"

namespace gc {

static uint32_t getMipLevels(uint32_t width, uint32_t height) { return static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1; }

static VkShaderModule createShaderModule(VkDevice device, std::span<const uint8_t> spv)
{
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;
    module_info.codeSize = spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shader_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &shader_module));
    return shader_module;
}

static VkSampleCountFlagBits getMaxSupportedSampleCount(const VkPhysicalDeviceLimits& limits, VkSampleCountFlagBits max)
{
    const auto check_support = [](const VkPhysicalDeviceLimits& limits, VkSampleCountFlagBits sample_count) {
        if (!(limits.framebufferColorSampleCounts & sample_count)) {
            return false;
        }
        if (!(limits.framebufferDepthSampleCounts & sample_count)) {
            return false;
        }
        if (!(limits.framebufferStencilSampleCounts & sample_count)) {
            return false;
        }
        return true;
    };
    VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : msaa_samples;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : msaa_samples;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT : msaa_samples;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_16_BIT) ? VK_SAMPLE_COUNT_16_BIT : msaa_samples;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_32_BIT) ? VK_SAMPLE_COUNT_32_BIT : msaa_samples;
    msaa_samples = check_support(limits, VK_SAMPLE_COUNT_64_BIT) ? VK_SAMPLE_COUNT_64_BIT : msaa_samples;
    msaa_samples = (msaa_samples > max) ? max : msaa_samples;
    return msaa_samples;
}

static uint32_t getAppropriateFramesInFlight(uint32_t swapchain_image_count) { return (swapchain_image_count > 2) ? 2 : 1; }

[[maybe_unused]] static void printGPUMemoryStats(VmaAllocator allocator, VkPhysicalDevice physical_device)
{
    VkPhysicalDeviceMemoryProperties2 mem_props{};
    mem_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    vkGetPhysicalDeviceMemoryProperties2(physical_device, &mem_props);

    std::vector<VmaBudget> heap_budgets(mem_props.memoryProperties.memoryHeapCount);
    vmaGetHeapBudgets(allocator, heap_budgets.data());

    GC_DEBUG("GPU memory heap budgets:");
    for (uint32_t i = 0; i < heap_budgets.size(); i++) {
        [[maybe_unused]] const VmaBudget& budget = heap_budgets[i];
        [[maybe_unused]] const VkMemoryHeap& heap = mem_props.memoryProperties.memoryHeaps[i];
        GC_DEBUG("  Memory heap {}", i);
        if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            GC_DEBUG("    GPU VRAM");
        }
        else {
            GC_DEBUG("    OTHER MEMORY");
        }
        GC_DEBUG("    Used {} ({} free)", bytesToHumanReadable(budget.usage), bytesToHumanReadable(budget.budget));
        GC_DEBUG("    Memory blocks allocated: {}", budget.statistics.blockCount);
        GC_DEBUG("    Number of allocations: {}", budget.statistics.allocationCount);
    }
}

// Must match FrameUniformBuffer in the shaders (std140 layout). A shader can declare only the first members, if that's all it needs.
struct PointLightUniformData {
    glm::vec4 position_range; // xyz = world position, w = range (zero: unlimited)
    glm::vec4 color;          // rgb = color * luminous intensity (candela)
};
struct FrameUniformData {
    glm::mat4 projection_matrix;
    glm::mat4 view_matrix;
    glm::vec3 camera_position;
    uint32_t point_light_count;
    glm::vec4 directional_light_direction; // xyz = direction towards the light
    glm::vec4 directional_light_color;     // rgb = color * illuminance (lux)
    float exposure;
    std::array<float, 3> padding;    // vectors are aligned to 16 bytes
    glm::vec4 ambient_light;         // rgb = color * illuminance (lux) of a surface facing up
    glm::mat4 shadow_matrix;         // world space to the shadow map's texture coordinates (x, y) and depth (z)
    glm::vec4 shadow_params;         // x = normal bias (metres), y = depth bias, z = 1 if there is a shadow map, else 0
    glm::mat4 environment_matrix;    // turns a direction in the world to one in the sky's cube maps
    glm::vec4 environment_params;    // x = the luminance (cd/m^2) of a texel of 1 when lighting with the sky, or 0 if the world isn't lit
                                     // by one. y = the last mip level of the sky's reflections. z = the same as x, for drawing the sky
    glm::vec4 fog_params;            // x = density (per metre, zero for no fog), y = height falloff (per metre), z = the height that x
                                     // is at, w = the luminance (cd/m^2) of a texel of 1 of the sky, if things fade to it, else 0
    glm::vec4 fog_color;             // rgb = luminance (cd/m^2), if things don't fade to the sky
    std::array<PointLightUniformData, WorldDrawData::MAX_POINT_LIGHTS> point_lights;
};
static_assert(offsetof(FrameUniformData, camera_position) == 128);
static_assert(offsetof(FrameUniformData, point_light_count) == 140);
static_assert(offsetof(FrameUniformData, directional_light_direction) == 144);
static_assert(offsetof(FrameUniformData, directional_light_color) == 160);
static_assert(offsetof(FrameUniformData, exposure) == 176);
static_assert(offsetof(FrameUniformData, ambient_light) == 192);
static_assert(offsetof(FrameUniformData, shadow_matrix) == 208);
static_assert(offsetof(FrameUniformData, shadow_params) == 272);
static_assert(offsetof(FrameUniformData, environment_matrix) == 288);
static_assert(offsetof(FrameUniformData, environment_params) == 352);
static_assert(offsetof(FrameUniformData, fog_params) == 368);
static_assert(offsetof(FrameUniformData, fog_color) == 384);
static_assert(offsetof(FrameUniformData, point_lights) == 400);
static_assert(sizeof(PointLightUniformData) == 32);

// Must match the fragment shader's push constant block
static_assert(offsetof(MaterialConstants, base_color) == 0);
static_assert(offsetof(MaterialConstants, roughness) == 16);
static_assert(offsetof(MaterialConstants, metallic) == 20);
static_assert(offsetof(MaterialConstants, alpha_cutoff) == 24);
static_assert(offsetof(MaterialConstants, emissive) == 32);

static_assert(MATERIAL_CONSTANTS_PUSH_CONSTANT_OFFSET + sizeof(MaterialConstants) <= TERRAIN_PUSH_CONSTANT_OFFSET);
static_assert(TERRAIN_PUSH_CONSTANT_OFFSET + sizeof(TerrainPushConstants) <= WORLD_PUSH_CONSTANTS_SIZE);

RenderBackend::RenderBackend(SDL_Window* window_handle)
    : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle), m_delete_queue(m_device.getHandle(), m_allocator.getHandle())
{
    // create sampler
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.mipLodBias = 0.0f;
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = 16.0f;
        info.compareEnable = VK_FALSE;
        info.minLod = 0.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        GC_CHECKVK(vkCreateSampler(m_device.getHandle(), &info, nullptr, &m_sampler));
    }

    // create main descriptor pool for long-lasting static resources
    {
        std::array<VkDescriptorPoolSize, 3> pool_sizes = {
            // every material has four textures. A shadow map has one and a sky has three, but there are only ever a few of those
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
            // the frame's uniforms
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
            // a terrain's tiles
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes) {
            pool_info.maxSets += pool_size.descriptorCount;
        }
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        GC_CHECKVK(vkCreateDescriptorPool(m_device.getHandle(), &pool_info, nullptr, &m_main_descriptor_pool));
    }

    // create descriptor set layouts
    {
        std::array<VkDescriptorSetLayoutBinding, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; // the fragment shader reads the lights
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_frame_set_layout));
    }
    {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        bindings[3].binding = 3; // emissive
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].pImmutableSamplers = &m_sampler;
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[0].pImmutableSamplers = &m_sampler;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].pImmutableSamplers = &m_sampler;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].pImmutableSamplers = &m_sampler;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size()); // base color texture and occlusion-roughness-metallic texture
        info.pBindings = bindings.data();
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_material_set_layout));
    }
    {
        // Shadow maps are read texel by texel (the shader does its own filtering), and never outside their edges
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        GC_CHECKVK(vkCreateSampler(m_device.getHandle(), &sampler_info, nullptr, &m_shadow_sampler));

        std::array<VkDescriptorSetLayoutBinding, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[0].pImmutableSamplers = &m_shadow_sampler;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_shadow_set_layout));
    }

    {
        // The sky's cube maps: the sky itself, its irradiance and its reflections. They are read with directions, so nothing
        // repeats, and the reflections are blurrier in each mip level, so the levels are blended.
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        GC_CHECKVK(vkCreateSampler(m_device.getHandle(), &sampler_info, nullptr, &m_environment_sampler));

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].pImmutableSamplers = &m_environment_sampler;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_environment_set_layout));
    }

    {
        // A terrain's tiles, which its vertex shader reads the heights from
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = &binding;
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_terrain_set_layout));
    }

    // The UI: one texture, and push constants (see UIPushConstants in gc_render_ui.cpp)
    {
        // Textures repeat, as RmlUi's tiled images go past the edges. No anisotropy: the UI faces the screen
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        GC_CHECKVK(vkCreateSampler(m_device.getHandle(), &sampler_info, nullptr, &m_ui_sampler));

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &m_ui_sampler;
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 1;
        set_info.pBindings = &binding;
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &set_info, nullptr, &m_ui_set_layout));

        VkPushConstantRange push_constant_range{};
        push_constant_range.offset = 0;
        push_constant_range.size = 128;
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &m_ui_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_constant_range;
        GC_CHECKVK(vkCreatePipelineLayout(m_device.getHandle(), &layout_info, nullptr, &m_ui_pipeline_layout));
    }

    // Pipeline layout. Every pipeline that draws the world uses it, so that descriptor sets stay bound when the pipeline changes.
    {
        const std::array set_layouts{m_frame_set_layout, m_material_set_layout, m_shadow_set_layout, m_environment_set_layout};

        // One range for both stages (see WORLD_PUSH_CONSTANT_STAGES): the world matrix (not used by instanced draws), the
        // material's constants, then a terrain's heights (only used by terrains, but in this layout too so that the terrain's is
        // compatible with it)
        std::array<VkPushConstantRange, 1> push_constant_ranges{};
        push_constant_ranges[0].offset = 0;
        push_constant_ranges[0].size = WORLD_PUSH_CONSTANTS_SIZE;
        push_constant_ranges[0].stageFlags = WORLD_PUSH_CONSTANT_STAGES;

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        info.pSetLayouts = set_layouts.data();
        info.pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size());
        info.pPushConstantRanges = push_constant_ranges.data();
        GC_CHECKVK(vkCreatePipelineLayout(m_device.getHandle(), &info, nullptr, &m_world_pipeline_layout));

        const std::array terrain_set_layouts{m_frame_set_layout, m_material_set_layout, m_shadow_set_layout, m_environment_set_layout, m_terrain_set_layout};
        info.setLayoutCount = static_cast<uint32_t>(terrain_set_layouts.size());
        info.pSetLayouts = terrain_set_layouts.data();
        GC_CHECKVK(vkCreatePipelineLayout(m_device.getHandle(), &info, nullptr, &m_terrain_pipeline_layout));
    }

    // find depth stencil format to use
    {
        // A floating point depth buffer, if there is one. The depth is reversed (1 at the near plane, 0 infinitely far away), which
        // with floats gives about the same precision at every distance. With 24 bit integers the precision runs out far away: a
        // few metres apart at a kilometre, which makes a terrain's distant hills flicker where they overlap.
        for (const VkFormat format : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
            VkFormatProperties depth_format_props{};
            vkGetPhysicalDeviceFormatProperties(m_device.getPhysicalDevice(), format, &depth_format_props);
            if (depth_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                m_depth_stencil_attachment_format = format;
                break;
            }
        }
        if (m_depth_stencil_attachment_format == VK_FORMAT_UNDEFINED) {
            abortGame("Failed to find suitable depth-buffer image format!");
        }
    }

    // choose number of MSAA samples to use
    if (m_device.getProperties().props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        m_msaa_samples = getMaxSupportedSampleCount(m_device.getProperties().props.properties.limits, VK_SAMPLE_COUNT_8_BIT);
    }
    else {
        m_msaa_samples = getMaxSupportedSampleCount(m_device.getProperties().props.properties.limits, VK_SAMPLE_COUNT_2_BIT); // MSAA is very slow on Intel iGPU
    }

    recreateRenderImages();

    m_requested_frames_in_flight = getAppropriateFramesInFlight(m_swapchain.getImageCount());

    {
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type_info;
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &info, nullptr, &m_main_timeline_semaphore));
        GC_CHECKVK(vkCreateSemaphore(m_device.getHandle(), &info, nullptr, &m_transfer_timeline_semaphore));
    }

    m_main_timeline_value = 0;
    m_framebuffer_copy_finished_value = 0;

    m_uploader = std::make_unique<RenderUploader>(m_device.getHandle(), m_allocator.getHandle(), m_delete_queue, m_device.getTransferQueue(),
                                                  m_device.getQueueFamilyIndex(), m_transfer_timeline_semaphore);

#ifdef TRACY_ENABLE
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = m_device.getQueueFamilyIndex();
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        GC_CHECKVK(vkCreateCommandPool(m_device.getHandle(), &info, nullptr, &m_tracy_vulkan_context.pool));
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandPool = m_tracy_vulkan_context.pool;
        cmd_info.commandBufferCount = 1;
        GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &cmd_info, &m_tracy_vulkan_context.cmd));
        if (m_device.isExtensionEnabled(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
            m_tracy_vulkan_context.ctx =
                TracyVkContextCalibrated(m_device.getPhysicalDevice(), m_device.getHandle(), m_device.getMainQueue(), m_tracy_vulkan_context.cmd,
                                         vkGetPhysicalDeviceCalibrateableTimeDomainsEXT, vkGetCalibratedTimestampsEXT);
        }
        else {
            m_tracy_vulkan_context.ctx =
                TracyVkContext(m_device.getPhysicalDevice(), m_device.getHandle(), m_device.getMainQueue(), m_tracy_vulkan_context.cmd);
        }
    }
#endif

    {
        // one texel at the farthest depth, which nothing is behind
        const std::array<uint8_t, 10> no_shadow_pak{1, 0, 0, 0, 1, 0, 0, 0, 0xff, 0xff};
        m_no_shadow_map = std::make_unique<RenderShadowMap>(createShadowMap(no_shadow_pak));
    }
    {
        // a cube map of one black texel a face (the header is the size of a face and the number of mip levels, see gcpak.h)
        std::array<uint8_t, ResourceCubeMap::HEADER_SIZE + 6 * sizeof(uint32_t)> no_sky_pak{};
        no_sky_pak[0] = 1;
        no_sky_pak[4] = 1;
        m_no_environment = std::make_unique<RenderEnvironment>(createEnvironment(no_sky_pak, no_sky_pak, no_sky_pak));
    }
    {
        const std::array<uint8_t, 4> white{0xff, 0xff, 0xff, 0xff};
        m_ui_white_texture = createUITexture(white, 1, 1, false);
    }

    // m_main_timeline_semaphore and the frame in flight command pools will be created when submitFrame() is called for the first time.

    VkPhysicalDeviceDriverProperties driverProps{};
    driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &driverProps;

    vkGetPhysicalDeviceProperties2(m_device.getPhysicalDevice(), &props2);

    GC_INFO("Driver name: {}", driverProps.driverName);
    GC_INFO("Driver info: {}", driverProps.driverInfo);

    GC_TRACE("Initialised RenderBackend");
}

RenderBackend::~RenderBackend()
{
    GC_TRACE("Destroying RenderBackend...");

    // The destructors for these objects defer destruction until the resource is no longer in use by a GPU queue.
    m_no_shadow_map.reset();
    m_no_environment.reset();
    m_ui_white_texture.reset();
    for (auto& pipeline : m_ui_pipelines) {
        pipeline.reset();
    }
    m_ui_vertex_buffer.reset();
    m_ui_index_buffer.reset();
    m_sky_pipeline.reset();
    m_frame_uniform_buffer_set.reset();
    m_instancing_transforms_buffer.reset();
    m_terrain_patches_buffer.reset();
    for (auto& pipeline : m_terrain_pipelines) {
        pipeline.reset();
    }
    m_frame_uniform_buffer.reset();
    for (auto& pipeline : m_main_pipelines) {
        pipeline.reset();
    }
    for (auto& pipeline : m_instancing_pipelines) {
        pipeline.reset();
    }

    waitIdle();

    cleanupGPUResources();
    if (!m_delete_queue.empty()) {
        GC_WARN("One or more GPU resources are still in use at application shutdown!");
        uint64_t main_val{}, transfer_val{};
        GC_CHECKVK(vkGetSemaphoreCounterValue(m_device.getHandle(), m_main_timeline_semaphore, &main_val));
        GC_CHECKVK(vkGetSemaphoreCounterValue(m_device.getHandle(), m_transfer_timeline_semaphore, &transfer_val));
        GC_TRACE("Main semaphore value: {}", main_val);
        GC_TRACE("Transfer semaphore value: {}", transfer_val);
    }

    // stops the render work thread
    m_uploader.reset();

    if (m_transfer_timeline_semaphore) {
        vkDestroySemaphore(m_device.getHandle(), m_transfer_timeline_semaphore, nullptr);
    }

    if (m_main_timeline_semaphore) {
        vkDestroySemaphore(m_device.getHandle(), m_main_timeline_semaphore, nullptr);
    }

    for (const auto& stuff : m_fif) {
        vkDestroyCommandPool(m_device.getHandle(), stuff.pool, nullptr);
    }

    vkDestroyImageView(m_device.getHandle(), m_framebuffer_image_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_framebuffer_image, m_framebuffer_image_allocation);
    vkDestroyImageView(m_device.getHandle(), m_color_attachment_image_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_color_attachment_image, m_color_attachment_allocation);
    vkDestroyImageView(m_device.getHandle(), m_depth_stencil_attachment_view, nullptr);
    vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil_attachment_image, m_depth_stencil_attachment_allocation);

    vkDestroyShaderModule(m_device.getHandle(), m_ui_fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_ui_vertex_module, nullptr);
    vkDestroyPipelineLayout(m_device.getHandle(), m_ui_pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_ui_set_layout, nullptr);
    vkDestroySampler(m_device.getHandle(), m_ui_sampler, nullptr);

    vkDestroyShaderModule(m_device.getHandle(), m_terrain_fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_terrain_vertex_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_sky_fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_sky_vertex_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_instanced_vertex_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), m_single_draw_vertex_module, nullptr);

    vkDestroyPipelineLayout(m_device.getHandle(), m_terrain_pipeline_layout, nullptr);
    vkDestroyPipelineLayout(m_device.getHandle(), m_world_pipeline_layout, nullptr);

    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_terrain_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_environment_set_layout, nullptr);
    vkDestroySampler(m_device.getHandle(), m_environment_sampler, nullptr);
    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_shadow_set_layout, nullptr);
    vkDestroySampler(m_device.getHandle(), m_shadow_sampler, nullptr);
    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_material_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_frame_set_layout, nullptr);
    vkDestroyDescriptorPool(m_device.getHandle(), m_main_descriptor_pool, nullptr);
    vkDestroySampler(m_device.getHandle(), m_sampler, nullptr);
}

void RenderBackend::setSyncMode(RenderSyncMode mode)
{
    switch (mode) {
    case RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED:
        m_swapchain.setRequestedPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR);
        break;
    case RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED:
        m_swapchain.setRequestedPresentMode(VK_PRESENT_MODE_FIFO_KHR);
        break;
    case RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED:
        m_swapchain.setRequestedPresentMode(VK_PRESENT_MODE_MAILBOX_KHR);
        break;
    case RenderSyncMode::VSYNC_OFF:
        m_swapchain.setRequestedPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR);
        break;
    }
}

void RenderBackend::submitFrame(bool window_resized, const WorldDrawData& world_draw_data, const UIDrawData* ui_draw_data,
                                bool (*post_render_callback)(VkCommandBuffer cmd))
{
    ZoneScoped;

    if (m_requested_frames_in_flight != static_cast<int>(m_fif.size())) {
        recreateFramesInFlightResources();
        GC_DEBUG("Using {} frames in flight", m_fif.size());
    }

    auto& stuff = m_fif[m_frame_count % m_fif.size()];

    // Before anything is drawn, so that what the render work thread has finished copying is given to the GPU as soon as possible
    m_uploader->submitFinishedUploads();

    createMissingPipelines(world_draw_data);

    // Wait for command buffer to be available
    waitForFrameReady();

    GC_CHECKVK(vkResetCommandPool(m_device.getHandle(), stuff.pool, 0));

    {
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GC_CHECKVK(vkBeginCommandBuffer(stuff.cmd, &info));
    }

    {
        /* Transition color attachment image to COLOR_ATTACHMENT_OPTIMAL layout */
        std::array<VkImageMemoryBarrier2, 3> to_attachment_barriers{};
        to_attachment_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_attachment_barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_attachment_barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
        to_attachment_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_attachment_barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_attachment_barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attachment_barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attachment_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[0].image = m_color_attachment_image;
        to_attachment_barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_attachment_barriers[0].subresourceRange.baseMipLevel = 0;
        to_attachment_barriers[0].subresourceRange.levelCount = 1;
        to_attachment_barriers[0].subresourceRange.baseArrayLayer = 0;
        to_attachment_barriers[0].subresourceRange.layerCount = 1;
        /* Transition depth stencil attachment image to VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL layout */
        to_attachment_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_attachment_barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_attachment_barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
        to_attachment_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        to_attachment_barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        to_attachment_barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attachment_barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        to_attachment_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[1].image = m_depth_stencil_attachment_image;
        to_attachment_barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        to_attachment_barriers[1].subresourceRange.baseMipLevel = 0;
        to_attachment_barriers[1].subresourceRange.levelCount = 1;
        to_attachment_barriers[1].subresourceRange.baseArrayLayer = 0;
        to_attachment_barriers[1].subresourceRange.layerCount = 1;
        /* Transition framebuffer image to COLOR_ATTACHMENT_OPTIMAL_LAYUOUT */
        to_attachment_barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_attachment_barriers[2].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_attachment_barriers[2].srcAccessMask = VK_ACCESS_2_NONE;
        to_attachment_barriers[2].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_attachment_barriers[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_attachment_barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attachment_barriers[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attachment_barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment_barriers[2].image = m_framebuffer_image;
        to_attachment_barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_attachment_barriers[2].subresourceRange.baseMipLevel = 0;
        to_attachment_barriers[2].subresourceRange.levelCount = 1;
        to_attachment_barriers[2].subresourceRange.baseArrayLayer = 0;
        to_attachment_barriers[2].subresourceRange.layerCount = 1;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_attachment_barriers.size());
        dep.pImageMemoryBarriers = to_attachment_barriers.data();
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    // The shadow map can only be used once it has been uploaded. Until then, and if there isn't one, nothing is shadowed.
    RenderShadowMap* shadow_map = world_draw_data.getShadowMap();
    const bool has_shadow_map = shadow_map && shadow_map->isUploaded();
    if (!has_shadow_map) {
        m_no_shadow_map->waitForUpload(); // a single texel, uploaded when the backend was created
        shadow_map = m_no_shadow_map.get();
    }

    // The same goes for the sky: until it is uploaded, the world is lit as if it had none (by its ambient lights)
    RenderEnvironment* environment = world_draw_data.getEnvironment();
    const bool has_environment = environment && environment->isUploaded() && world_draw_data.getEnvironmentScale() > 0.0f;
    if (!has_environment) {
        m_no_environment->waitForUpload();
        environment = m_no_environment.get();
    }
    const bool draw_sky = has_environment && world_draw_data.getEnvironmentVisible();
    if (draw_sky && !m_sky_pipeline) {
        if (m_sky_vertex_module) {
            GC_DEBUG("Creating the pipeline that draws the sky");
            m_sky_pipeline = std::make_unique<GPUPipeline>(createSkyPipeline());
        }
        else {
            GC_WARN_ONCE("The world has a sky, but it can't be drawn as setSkyShaders() wasn't called");
        }
    }

    // update frame uniform buffer
    {
        FrameUniformData data{};
        data.projection_matrix = world_draw_data.getProjectionMatrix();
        data.view_matrix = world_draw_data.getViewMatrix();
        data.camera_position =
            glm::vec3(glm::inverse(data.view_matrix) * glm::vec4(0.0, 0.0, 0.0, 1.0)); // TODO, have world_draw_data just contain the camera position

        data.directional_light_direction = glm::vec4(world_draw_data.getDirectionalLightDirection(), 0.0f);
        data.directional_light_color = glm::vec4(world_draw_data.getDirectionalLightColor(), 0.0f);
        data.exposure = world_draw_data.getExposure();
        data.ambient_light = glm::vec4(world_draw_data.getAmbientLight(), 0.0f);
        data.shadow_matrix = world_draw_data.getShadowMatrix();
        data.shadow_params = glm::vec4(world_draw_data.getShadowNormalBias(), world_draw_data.getShadowDepthBias(), has_shadow_map ? 1.0f : 0.0f, 0.0f);
        data.environment_matrix = world_draw_data.getEnvironmentMatrix();
        if (has_environment) {
            const float scale = world_draw_data.getEnvironmentScale();
            data.environment_params = glm::vec4(world_draw_data.getEnvironmentLighting() ? scale : 0.0f,
                                                static_cast<float>(environment->getSpecularLevelCount() - 1), scale, 0.0f);
        }
        {
            const WorldFog& fog = world_draw_data.getFog();
            const bool fade_to_sky = fog.use_sky && has_environment;
            data.fog_params = glm::vec4(std::max(fog.density, 0.0f), std::max(fog.height_falloff, 0.0f), fog.base_height,
                                        fade_to_sky ? world_draw_data.getEnvironmentScale() : 0.0f);
            data.fog_color = glm::vec4(fog.color, 0.0f);
        }

        const auto& point_lights = world_draw_data.getPointLights();
        if (point_lights.size() > data.point_lights.size()) {
            GC_WARN_ONCE("The world has {} point lights but only {} can be drawn. The rest are left out", point_lights.size(), data.point_lights.size());
        }
        data.point_light_count = static_cast<uint32_t>(std::min(point_lights.size(), data.point_lights.size()));
        for (uint32_t i = 0; i < data.point_light_count; ++i) {
            data.point_lights[i].position_range = glm::vec4(point_lights[i].position, point_lights[i].range);
            data.point_lights[i].color = glm::vec4(point_lights[i].color, 0.0f);
        }

        m_frame_uniform_buffer->writeData(stuff.cmd, m_frame_count, m_main_timeline_semaphore, m_main_timeline_value + 1,
                                          std::span(reinterpret_cast<const uint8_t*>(&data), sizeof(data)));

        VkBufferMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_frame_uniform_buffer->getBuffer();
        b.size = VkDeviceSize(sizeof(data));
        b.offset = 0;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    if (!world_draw_data.getInstancedDrawTransforms().empty()) {
        TracyVkZone(m_tracy_vulkan_context.ctx, stuff.cmd, "Copy instance transforms");

        const auto& transforms = world_draw_data.getInstancedDrawTransforms();
        const size_t buffer_data_size = transforms.size() * sizeof(transforms[0]);
        const std::span<const uint8_t> transform_data(reinterpret_cast<const uint8_t*>(transforms.data()), buffer_data_size);
        m_instancing_transforms_buffer->writeData(stuff.cmd, m_frame_count, m_main_timeline_semaphore, m_main_timeline_value + 1, transform_data);

        VkBufferMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_instancing_transforms_buffer->getBuffer();
        b.size = VkDeviceSize(buffer_data_size);
        b.offset = 0;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    if (!world_draw_data.getTerrainPatches().empty()) {
        const auto& patches = world_draw_data.getTerrainPatches();
        const size_t buffer_data_size = patches.size() * sizeof(patches[0]);
        m_terrain_patches_buffer->writeData(stuff.cmd, m_frame_count, m_main_timeline_semaphore, m_main_timeline_value + 1,
                                            std::span(reinterpret_cast<const uint8_t*>(patches.data()), buffer_data_size));

        VkBufferMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_terrain_patches_buffer->getBuffer();
        b.size = VkDeviceSize(buffer_data_size);
        b.offset = 0;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    const bool draw_ui = ui_draw_data && !ui_draw_data->empty() && hasUIShaders();
    if (draw_ui) {
        TracyVkZone(m_tracy_vulkan_context.ctx, stuff.cmd, "Copy UI geometry");

        const auto& vertices = ui_draw_data->getVertices();
        const auto& indices = ui_draw_data->getIndices();
        m_ui_vertex_buffer->writeData(stuff.cmd, m_frame_count, m_main_timeline_semaphore, m_main_timeline_value + 1,
                                      std::span(reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(vertices[0])));
        m_ui_index_buffer->writeData(stuff.cmd, m_frame_count, m_main_timeline_semaphore, m_main_timeline_value + 1,
                                     std::span(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(indices[0])));

        std::array<VkBufferMemoryBarrier2, 2> barriers{};
        for (VkBufferMemoryBarrier2& b : barriers) {
            b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.offset = 0;
        }
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barriers[0].buffer = m_ui_vertex_buffer->getBuffer();
        barriers[0].size = VkDeviceSize(vertices.size() * sizeof(vertices[0]));
        barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        barriers[1].buffer = m_ui_index_buffer->getBuffer();
        barriers[1].size = VkDeviceSize(indices.size() * sizeof(indices[0]));
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    {
        ZoneScopedN("Record render commands");
        ZoneValue(m_frame_count);
        TracyVkZone(m_tracy_vulkan_context.ctx, stuff.cmd, "Render to framebuffer");

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = m_color_attachment_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_attachment.resolveImageView = m_framebuffer_image_view;
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.clearValue.color.float32[0] = 1.0f;
        color_attachment.clearValue.color.float32[1] = 1.0f;
        color_attachment.clearValue.color.float32[2] = 1.0f;
        color_attachment.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = m_depth_stencil_attachment_view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 0.0f;
        // The stencil is only used by the UI, for its clip masks (the depth stencil format always has one)
        VkRenderingAttachmentInfo stencil_attachment = depth_attachment;
        stencil_attachment.clearValue.depthStencil.stencil = 0;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.offset.x = 0;
        info.renderArea.offset.y = 0;
        info.renderArea.extent = m_swapchain.getExtent();
        info.layerCount = 1;
        info.viewMask = 0;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color_attachment;
        info.pDepthAttachment = &depth_attachment;
        info.pStencilAttachment = &stencil_attachment;
        vkCmdBeginRendering(stuff.cmd, &info);

        // Set viewport and scissor (dynamic states)
        const VkExtent2D swapchain_extent = m_swapchain.getExtent();
        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapchain_extent.width);
        viewport.height = static_cast<float>(swapchain_extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(stuff.cmd, 0, 1, &viewport);
        VkRect2D scissor = {};
        scissor.offset = {0, 0};
        scissor.extent = swapchain_extent;
        vkCmdSetScissor(stuff.cmd, 0, 1, &scissor);

        recordWorldRenderingCommands(stuff.cmd, m_world_pipeline_layout, m_main_pipelines, m_instancing_pipelines, m_main_timeline_semaphore,
                                     m_main_timeline_value + 1, world_draw_data, *m_frame_uniform_buffer_set, *shadow_map, *environment,
                                     draw_sky ? m_sky_pipeline.get() : nullptr, *m_instancing_transforms_buffer, m_terrain_pipeline_layout,
                                     m_terrain_pipelines, *m_terrain_patches_buffer);

        if (draw_ui) {
            TracyVkZone(m_tracy_vulkan_context.ctx, stuff.cmd, "Draw UI");
            recordUIRenderingCommands(stuff.cmd, m_device.getHandle(), m_ui_pipeline_layout, m_ui_pipelines, *m_ui_white_texture, *m_ui_vertex_buffer,
                                      *m_ui_index_buffer, swapchain_extent, m_main_timeline_semaphore, m_main_timeline_value + 1, *ui_draw_data);
        }

        if (post_render_callback) {
            bool ret = post_render_callback(stuff.cmd);
            if (!ret) {
                GC_ERROR_ONCE("Post render callback returned error");
            }
        }

        vkCmdEndRendering(stuff.cmd);
    }

    /* Transition image to TRANSFER_SRC layout */
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_framebuffer_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(stuff.cmd, &dep);
    }

    TracyVkCollect(m_tracy_vulkan_context.ctx, stuff.cmd);

    GC_CHECKVK(vkEndCommandBuffer(stuff.cmd));

    /* Submit command buffer */
    {
        ZoneScopedN("Submit command buffer, signal with:");
        ZoneValue(m_main_timeline_value + 1);

        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = stuff.cmd;

        // Wait for the framebuffer image to finish being copied from to a swapchain image
        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = m_main_timeline_semaphore;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        wait_info.value = m_framebuffer_copy_finished_value;

        m_main_timeline_value += 1;
        stuff.command_buffer_available_value = m_main_timeline_value;

        // Waited on by m_swapchain.acquireAndPresent()'s copy operation just after this queueSubmit
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_main_timeline_semaphore;
        signal_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        signal_info.value = m_main_timeline_value;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait_info;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal_info;
        GC_CHECKVK(vkQueueSubmit2(m_device.getMainQueue(), 1, &submit, VK_NULL_HANDLE));
    }

#ifdef TRACY_ENABLE
    const TracyVkCtx tracy_ctx = m_tracy_vulkan_context.ctx;
#else
    const TracyVkCtx tracy_ctx{};
#endif
    const bool swapchain_recreated =
        m_swapchain.acquireAndPresent(m_framebuffer_image, window_resized, m_main_timeline_semaphore, m_main_timeline_value, tracy_ctx);

    m_framebuffer_copy_finished_value = m_main_timeline_value;

    if (swapchain_recreated) {
        GC_CHECKVK(
            vkQueueWaitIdle(m_device.getMainQueue())); // if window was just un-minimised, acquireAndPresent() will be using the framebuffer image right now.
        recreateRenderImages();
        m_requested_frames_in_flight = getAppropriateFramesInFlight(m_swapchain.getImageCount());
    }

    ++m_frame_count;
}

void RenderBackend::cleanupGPUResources()
{
    ZoneScoped;
    auto num_resources_deleted = m_delete_queue.deleteUnusedResources(std::array{m_main_timeline_semaphore, m_transfer_timeline_semaphore});
    if (num_resources_deleted > 0) {
        GC_DEBUG("Deleted {} GPU resources", num_resources_deleted);
        // printGPUMemoryStats(m_allocator.getHandle(), m_device.getPhysicalDevice());
    }
}

void RenderBackend::setWorldShaders(std::span<const uint8_t> single_draw_vertex_spv, std::span<const uint8_t> instanced_vertex_spv,
                                    std::span<const uint8_t> fragment_spv)
{
    if (m_fragment_module) {
        abortGame("World shaders already set!");
    }
    if (single_draw_vertex_spv.empty() || instanced_vertex_spv.empty() || fragment_spv.empty()) {
        abortGame("setWorldShaders() called with empty SPIRV code");
    }

    m_single_draw_vertex_module = createShaderModule(m_device.getHandle(), single_draw_vertex_spv);
    m_instanced_vertex_module = createShaderModule(m_device.getHandle(), instanced_vertex_spv);
    m_fragment_module = createShaderModule(m_device.getHandle(), fragment_spv);
}

void RenderBackend::setSkyShaders(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv)
{
    if (m_sky_vertex_module) {
        abortGame("Sky shaders already set!");
    }
    if (vertex_spv.empty() || fragment_spv.empty()) {
        abortGame("setSkyShaders() called with empty SPIRV code");
    }
    m_sky_vertex_module = createShaderModule(m_device.getHandle(), vertex_spv);
    m_sky_fragment_module = createShaderModule(m_device.getHandle(), fragment_spv);
}

void RenderBackend::setTerrainShaders(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv)
{
    if (m_terrain_vertex_module) {
        abortGame("Terrain shaders already set!");
    }
    if (vertex_spv.empty() || fragment_spv.empty()) {
        abortGame("setTerrainShaders() called with empty SPIRV code");
    }
    m_terrain_vertex_module = createShaderModule(m_device.getHandle(), vertex_spv);
    m_terrain_fragment_module = createShaderModule(m_device.getHandle(), fragment_spv);
}

void RenderBackend::setUIShaders(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv)
{
    if (m_ui_vertex_module) {
        abortGame("UI shaders already set!");
    }
    if (vertex_spv.empty() || fragment_spv.empty()) {
        abortGame("setUIShaders() called with empty SPIRV code");
    }
    m_ui_vertex_module = createShaderModule(m_device.getHandle(), vertex_spv);
    m_ui_fragment_module = createShaderModule(m_device.getHandle(), fragment_spv);
    for (uint32_t i = 0; i < static_cast<uint32_t>(UIPipelineKind::COUNT); ++i) {
        m_ui_pipelines[i] = std::make_unique<GPUPipeline>(createUIPipeline(static_cast<UIPipelineKind>(i)));
    }
}

std::unique_ptr<RenderUITexture> RenderBackend::createUITexture(std::span<const uint8_t> texels, uint32_t width, uint32_t height, bool generate_mips)
{
    ZoneScoped;

    GC_ASSERT(width != 0 && height != 0);
    GC_ASSERT(texels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    // UNORM: the shader decodes the sRGB itself, as the alpha has to be taken out first
    const uint32_t mip_levels = generate_mips ? getMipLevels(width, height) : 1;
    RenderTexture texture(m_uploader->uploadImage(VK_FORMAT_R8G8B8A8_UNORM, width, height, mip_levels, false, {texels}, RenderUploadMode::IMMEDIATE));

    VkDescriptorSet descriptor_set{};
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_main_descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &m_ui_set_layout;
    GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &descriptor_set));

    return std::make_unique<RenderUITexture>(std::move(texture), GPUDescriptorSet(m_delete_queue, m_main_descriptor_pool, descriptor_set));
}

// RmlUi's triangles, in pixels. Drawn without depth, in the order they are given
GPUPipeline RenderBackend::createUIPipeline(UIPipelineKind kind)
{
    ZoneScoped;

    std::array<VkPipelineShaderStageCreateInfo, 2> stage_infos{};
    stage_infos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_infos[0].pName = "main";
    stage_infos[1] = stage_infos[0];
    stage_infos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage_infos[0].module = m_ui_vertex_module;
    stage_infos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage_infos[1].module = m_ui_fragment_module;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(UIVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(UIVertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R8G8B8A8_UNORM;
    attributes[1].offset = offsetof(UIVertex, color);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(UIVertex, uv);
    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.vertexBindingDescriptionCount = 1;
    vertex_input_state.pVertexBindingDescriptions = &binding;
    vertex_input_state.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input_state.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization_state{};
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.lineWidth = 1.0f;
    rasterization_state.cullMode = VK_CULL_MODE_NONE; // (transforms can flip triangles over)
    rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    const VkFormat color_attachment_format = m_swapchain.getSurfaceFormat().format;
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = m_depth_stencil_attachment_format;
    rendering_info.stencilAttachmentFormat = m_depth_stencil_attachment_format;

    VkPipelineMultisampleStateCreateInfo multisample_state{};
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = m_msaa_samples;
    multisample_state.minSampleShading = 1.0f;

    // The stencil buffer holds the clip mask. RmlUi's clip masks work as in its OpenGL 3 renderer: set it to the reference value
    // (after clearing it), add 1 to intersect, and draw where it is the reference value.
    const bool writes_mask = (kind == UIPipelineKind::CLIP_REPLACE || kind == UIPipelineKind::CLIP_INCREMENT);
    VkStencilOpState stencil{};
    stencil.failOp = VK_STENCIL_OP_KEEP;
    stencil.depthFailOp = VK_STENCIL_OP_KEEP;
    stencil.compareMask = 0xff;
    switch (kind) {
    case UIPipelineKind::DRAW:
    case UIPipelineKind::COUNT:
        stencil.passOp = VK_STENCIL_OP_KEEP;
        stencil.compareOp = VK_COMPARE_OP_ALWAYS;
        break;
    case UIPipelineKind::DRAW_CLIPPED:
        stencil.passOp = VK_STENCIL_OP_KEEP;
        stencil.compareOp = VK_COMPARE_OP_EQUAL;
        break;
    case UIPipelineKind::CLIP_REPLACE:
        stencil.passOp = VK_STENCIL_OP_REPLACE;
        stencil.compareOp = VK_COMPARE_OP_ALWAYS;
        stencil.writeMask = 0xff;
        break;
    case UIPipelineKind::CLIP_INCREMENT:
        stencil.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        stencil.compareOp = VK_COMPARE_OP_ALWAYS;
        stencil.writeMask = 0xff;
        break;
    }
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};
    depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = VK_FALSE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    depth_stencil_state.stencilTestEnable = (kind == UIPipelineKind::DRAW) ? VK_FALSE : VK_TRUE;
    depth_stencil_state.front = stencil;
    depth_stencil_state.back = stencil;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;

    // Premultiplied alpha, as RmlUi's colors and textures are
    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask =
        writes_mask ? 0 : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    color_blend_attachment.blendEnable = writes_mask ? VK_FALSE : VK_TRUE;
    color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo color_blend_state{};
    color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.attachmentCount = 1;
    color_blend_state.pAttachments = &color_blend_attachment;

    std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering_info;
    info.stageCount = static_cast<uint32_t>(stage_infos.size());
    info.pStages = stage_infos.data();
    info.pVertexInputState = &vertex_input_state;
    info.pInputAssemblyState = &input_assembly_state;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization_state;
    info.pMultisampleState = &multisample_state;
    info.pDepthStencilState = &depth_stencil_state;
    info.pColorBlendState = &color_blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = m_ui_pipeline_layout;
    info.basePipelineIndex = -1;

    VkPipeline handle{};
    GC_CHECKVK(vkCreateGraphicsPipelines(m_device.getHandle(), VK_NULL_HANDLE, 1, &info, nullptr, &handle));

    return GPUPipeline(m_delete_queue, handle);
}

// One triangle, made by the vertex shader, that is drawn wherever the depth buffer still has nothing in it
GPUPipeline RenderBackend::createSkyPipeline()
{
    ZoneScoped;

    std::array<VkPipelineShaderStageCreateInfo, 2> stage_infos{};
    stage_infos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_infos[0].pName = "main";
    stage_infos[1] = stage_infos[0];
    stage_infos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage_infos[0].module = m_sky_vertex_module;
    stage_infos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage_infos[1].module = m_sky_fragment_module;

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization_state{};
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.lineWidth = 1.0f;
    rasterization_state.cullMode = VK_CULL_MODE_NONE;
    rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    const VkFormat color_attachment_format = m_swapchain.getSurfaceFormat().format;
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = m_depth_stencil_attachment_format;
    rendering_info.stencilAttachmentFormat = m_depth_stencil_attachment_format;

    VkPipelineMultisampleStateCreateInfo multisample_state{};
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = m_msaa_samples;
    multisample_state.minSampleShading = 1.0f;

    // The far plane is at a depth of zero, which is also what the depth buffer is cleared to. So the sky passes wherever nothing
    // has been drawn, and nowhere else. It doesn't write depth: blended things in front of it are drawn afterwards anyway.
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};
    depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_FALSE;
    depth_stencil_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo color_blend_state{};
    color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.attachmentCount = 1;
    color_blend_state.pAttachments = &color_blend_attachment;

    std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering_info;
    info.stageCount = static_cast<uint32_t>(stage_infos.size());
    info.pStages = stage_infos.data();
    info.pVertexInputState = &vertex_input_state;
    info.pInputAssemblyState = &input_assembly_state;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization_state;
    info.pMultisampleState = &multisample_state;
    info.pDepthStencilState = &depth_stencil_state;
    info.pColorBlendState = &color_blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = m_world_pipeline_layout;
    info.basePipelineIndex = -1;

    VkPipeline handle{};
    GC_CHECKVK(vkCreateGraphicsPipelines(m_device.getHandle(), VK_NULL_HANDLE, 1, &info, nullptr, &handle));

    return GPUPipeline(m_delete_queue, handle);
}

GPUPipeline RenderBackend::createPipeline(VkShaderModule vertex_module, VkShaderModule fragment_module, uint32_t variant,
                                          const VkPipelineVertexInputStateCreateInfo& vertex_input_state, VkPipelineLayout layout)
{
    ZoneScoped;

    if (!vertex_module || !fragment_module) {
        abortGame("setWorldShaders() must be called before the world is drawn");
    }
    GC_ASSERT(variant < MATERIAL_PIPELINE_VARIANTS);
    const MaterialFeatures features = variant % MATERIAL_FEATURE_COMBINATIONS;
    const MaterialBlendMode blend_mode = static_cast<MaterialBlendMode>(variant / MATERIAL_FEATURE_COMBINATIONS);

    // The fragment shader has a bool specialization constant for each material feature. The driver compiles out what a variant
    // doesn't use, such as sampling a normal map.
    // The constant after those is the blend mode: the shader discards what fails the alpha test, or keeps the alpha for blending.
    static_assert(sizeof(VkBool32) == sizeof(uint32_t));
    std::array<uint32_t, MATERIAL_FEATURE_COUNT + 1> specialization_data{};
    std::array<VkSpecializationMapEntry, MATERIAL_FEATURE_COUNT + 1> specialization_entries{};
    specialization_data[MATERIAL_FEATURE_COUNT] = static_cast<uint32_t>(blend_mode);
    specialization_entries[MATERIAL_FEATURE_COUNT].constantID = MATERIAL_FEATURE_COUNT;
    specialization_entries[MATERIAL_FEATURE_COUNT].offset = MATERIAL_FEATURE_COUNT * static_cast<uint32_t>(sizeof(uint32_t));
    specialization_entries[MATERIAL_FEATURE_COUNT].size = sizeof(uint32_t);
    for (uint32_t i = 0; i < MATERIAL_FEATURE_COUNT; ++i) {
        specialization_data[i] = (features & (1u << i)) ? VK_TRUE : VK_FALSE;
        specialization_entries[i].constantID = i;
        specialization_entries[i].offset = i * static_cast<uint32_t>(sizeof(VkBool32));
        specialization_entries[i].size = sizeof(VkBool32);
    }
    VkSpecializationInfo specialization_info{};
    specialization_info.mapEntryCount = static_cast<uint32_t>(specialization_entries.size());
    specialization_info.pMapEntries = specialization_entries.data();
    specialization_info.dataSize = sizeof(specialization_data);
    specialization_info.pData = specialization_data.data();

    std::array<VkPipelineShaderStageCreateInfo, 2> stage_infos{};
    stage_infos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_infos[0].pNext = nullptr;
    stage_infos[0].flags = 0;
    stage_infos[0].pName = "main";
    stage_infos[0].pSpecializationInfo = nullptr;
    stage_infos[1] = stage_infos[0];
    stage_infos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage_infos[0].module = vertex_module;
    stage_infos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage_infos[1].module = fragment_module;
    stage_infos[1].pSpecializationInfo = &specialization_info;

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.pNext = nullptr;
    input_assembly_state.flags = 0;
    input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_state.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = nullptr;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = nullptr;

    VkPipelineRasterizationStateCreateInfo rasterization_state{};
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.depthClampEnable = VK_FALSE;
    rasterization_state.rasterizerDiscardEnable = VK_FALSE; // enabling this will not run the fragment shaders at all
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.lineWidth = 1.0f;
    rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization_state.depthBiasEnable = VK_FALSE;
    rasterization_state.depthBiasConstantFactor = 0.0f; // ignored
    rasterization_state.depthBiasClamp = 0.0f;          // ignored
    rasterization_state.depthBiasSlopeFactor = 0.0f;    // ignored

    const VkFormat color_attachment_format = m_swapchain.getSurfaceFormat().format;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = m_depth_stencil_attachment_format;
    rendering_info.stencilAttachmentFormat = m_depth_stencil_attachment_format;

    VkPipelineMultisampleStateCreateInfo multisample_state{};
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = m_msaa_samples;
    multisample_state.sampleShadingEnable = VK_FALSE;
    multisample_state.minSampleShading = 1.0f;          // ignored
    multisample_state.pSampleMask = nullptr;            // ignored
    multisample_state.alphaToCoverageEnable = VK_FALSE; // ignored
    multisample_state.alphaToOneEnable = VK_FALSE;      // ignored

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};
    depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = VK_TRUE;
    // Blended surfaces are hidden by what is in front of them, but don't hide what is behind them
    depth_stencil_state.depthWriteEnable = (blend_mode == MaterialBlendMode::ALPHA_BLEND) ? VK_FALSE : VK_TRUE;
    depth_stencil_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;
    depth_stencil_state.stencilTestEnable = VK_FALSE;
    depth_stencil_state.front = {};
    depth_stencil_state.back = {};

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend_mode == MaterialBlendMode::ALPHA_BLEND) {
        // color = color * alpha + what is there already * (1 - alpha)
        color_blend_attachment.blendEnable = VK_TRUE;
        color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    else {
        color_blend_attachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo color_blend_state{};
    color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.logicOpEnable = VK_FALSE;
    color_blend_state.logicOp = VK_LOGIC_OP_COPY; // ignored
    color_blend_state.attachmentCount = 1;
    color_blend_state.pAttachments = &color_blend_attachment;
    color_blend_state.blendConstants[0] = 0.0f; // ignored
    color_blend_state.blendConstants[1] = 0.0f; // ignored
    color_blend_state.blendConstants[2] = 0.0f; // ignored
    color_blend_state.blendConstants[3] = 0.0f; // ignored

    std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext = nullptr;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering_info;
    info.flags = 0;
    info.stageCount = static_cast<uint32_t>(stage_infos.size());
    info.pStages = stage_infos.data();
    info.pVertexInputState = &vertex_input_state;
    info.pInputAssemblyState = &input_assembly_state;
    info.pTessellationState = nullptr;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization_state;
    info.pMultisampleState = &multisample_state;
    info.pDepthStencilState = &depth_stencil_state;
    info.pColorBlendState = &color_blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = layout;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = -1;

    VkPipeline handle{};
    GC_CHECKVK(vkCreateGraphicsPipelines(m_device.getHandle(), VK_NULL_HANDLE, 1, &info, nullptr, &handle));

    return GPUPipeline(m_delete_queue, handle);
}

GPUPipeline RenderBackend::createMainPipeline(uint32_t variant)
{
    VkVertexInputBindingDescription vertex_input_binding{};
    vertex_input_binding.binding = 0;
    vertex_input_binding.stride = static_cast<uint32_t>(sizeof(MeshVertex));
    vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> vertex_input_attributes{};
    vertex_input_attributes[0].binding = 0;
    vertex_input_attributes[0].location = 0;
    vertex_input_attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_attributes[0].offset = static_cast<uint32_t>(offsetof(MeshVertex, position));

    vertex_input_attributes[1].binding = 0;
    vertex_input_attributes[1].location = 1;
    vertex_input_attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_attributes[1].offset = static_cast<uint32_t>(offsetof(MeshVertex, normal));

    vertex_input_attributes[2].binding = 0;
    vertex_input_attributes[2].location = 2;
    vertex_input_attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[2].offset = static_cast<uint32_t>(offsetof(MeshVertex, tangent));

    vertex_input_attributes[3].binding = 0;
    vertex_input_attributes[3].location = 3;
    vertex_input_attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_input_attributes[3].offset = static_cast<uint32_t>(offsetof(MeshVertex, uv));

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.pNext = nullptr;
    vertex_input_state.flags = 0;
    vertex_input_state.vertexBindingDescriptionCount = 1;
    vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
    vertex_input_state.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_input_attributes.size());
    vertex_input_state.pVertexAttributeDescriptions = vertex_input_attributes.data();

    return createPipeline(m_single_draw_vertex_module, m_fragment_module, variant, vertex_input_state, m_world_pipeline_layout);
}

GPUPipeline RenderBackend::createInstancingPipeline(uint32_t variant)
{
    std::array<VkVertexInputBindingDescription, 2> vertex_input_bindings{};

    vertex_input_bindings[0].binding = 0;
    vertex_input_bindings[0].stride = static_cast<uint32_t>(sizeof(MeshVertex));
    vertex_input_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    vertex_input_bindings[1].binding = 1;
    vertex_input_bindings[1].stride = static_cast<uint32_t>(sizeof(glm::mat4));
    vertex_input_bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 8> vertex_input_attributes{};

    vertex_input_attributes[0].binding = 0;
    vertex_input_attributes[0].location = 0;
    vertex_input_attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_attributes[0].offset = static_cast<uint32_t>(offsetof(MeshVertex, position));

    vertex_input_attributes[1].binding = 0;
    vertex_input_attributes[1].location = 1;
    vertex_input_attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_attributes[1].offset = static_cast<uint32_t>(offsetof(MeshVertex, normal));

    vertex_input_attributes[2].binding = 0;
    vertex_input_attributes[2].location = 2;
    vertex_input_attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[2].offset = static_cast<uint32_t>(offsetof(MeshVertex, tangent));

    vertex_input_attributes[3].binding = 0;
    vertex_input_attributes[3].location = 3;
    vertex_input_attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_input_attributes[3].offset = static_cast<uint32_t>(offsetof(MeshVertex, uv));

    vertex_input_attributes[4].binding = 1;
    vertex_input_attributes[4].location = 4;
    vertex_input_attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[4].offset = static_cast<uint32_t>(sizeof(glm::vec4) * 0);

    vertex_input_attributes[5].binding = 1;
    vertex_input_attributes[5].location = 5;
    vertex_input_attributes[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[5].offset = static_cast<uint32_t>(sizeof(glm::vec4) * 1);

    vertex_input_attributes[6].binding = 1;
    vertex_input_attributes[6].location = 6;
    vertex_input_attributes[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[6].offset = static_cast<uint32_t>(sizeof(glm::vec4) * 2);

    vertex_input_attributes[7].binding = 1;
    vertex_input_attributes[7].location = 7;
    vertex_input_attributes[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_input_attributes[7].offset = static_cast<uint32_t>(sizeof(glm::vec4) * 3);

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.pNext = nullptr;
    vertex_input_state.flags = 0;
    vertex_input_state.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_input_bindings.size());
    vertex_input_state.pVertexBindingDescriptions = vertex_input_bindings.data();
    vertex_input_state.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_input_attributes.size());
    vertex_input_state.pVertexAttributeDescriptions = vertex_input_attributes.data();

    return createPipeline(m_instanced_vertex_module, m_fragment_module, variant, vertex_input_state, m_world_pipeline_layout);
}

GPUPipeline RenderBackend::createTerrainPipeline(MaterialFeatures features)
{
    if (!m_terrain_vertex_module || !m_terrain_fragment_module) {
        abortGame("setTerrainShaders() must be called before a terrain is drawn");
    }

    // There is no vertex buffer, only the patches, one per instance (see TerrainPatch)
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<uint32_t>(sizeof(TerrainPatch));
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32A32_UINT;
    attributes[0].offset = static_cast<uint32_t>(offsetof(TerrainPatch, tile));
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[1].offset = static_cast<uint32_t>(offsetof(TerrainPatch, x));
    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = static_cast<uint32_t>(offsetof(TerrainPatch, morph_start));

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.vertexBindingDescriptionCount = 1;
    vertex_input_state.pVertexBindingDescriptions = &binding;
    vertex_input_state.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input_state.pVertexAttributeDescriptions = attributes.data();

    return createPipeline(m_terrain_vertex_module, m_terrain_fragment_module, getMaterialPipelineVariant(features, MaterialBlendMode::NONE),
                          vertex_input_state, m_terrain_pipeline_layout);
}

void RenderBackend::createMissingPipelines(const WorldDrawData& world_draw_data)
{
    for (const auto& entry : world_draw_data.getDrawEntries()) {
        const uint32_t variant = entry.material->getPipelineVariant();
        if (!m_main_pipelines[variant]) [[unlikely]] {
            GC_DEBUG("Creating pipeline for material features {:#05b}, blend mode {}", entry.material->getFeatures(),
                     static_cast<uint32_t>(entry.material->getBlendMode()));
            m_main_pipelines[variant] = std::make_unique<GPUPipeline>(createMainPipeline(variant));
        }
    }
    for (const auto& entry : world_draw_data.getInstancedDrawEntries()) {
        const uint32_t variant = entry.material->getPipelineVariant();
        if (!m_instancing_pipelines[variant]) [[unlikely]] {
            GC_DEBUG("Creating instancing pipeline for material features {:#05b}, blend mode {}", entry.material->getFeatures(),
                     static_cast<uint32_t>(entry.material->getBlendMode()));
            m_instancing_pipelines[variant] = std::make_unique<GPUPipeline>(createInstancingPipeline(variant));
        }
    }
    for (const auto& entry : world_draw_data.getTerrainDrawEntries()) {
        const MaterialFeatures features = entry.material->getFeatures();
        if (!m_terrain_pipelines[features]) [[unlikely]] {
            GC_DEBUG("Creating terrain pipeline for material features {:#05b}", features);
            m_terrain_pipelines[features] = std::make_unique<GPUPipeline>(createTerrainPipeline(features));
        }
    }
}

RenderTexture RenderBackend::createTexture(std::span<const uint8_t> r8g8b8a8_pak, bool srgb, RenderUploadMode upload_mode)
{
    return createTextureFromPak(r8g8b8a8_pak, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4, true, upload_mode);
}

RenderShadowMap RenderBackend::createShadowMap(std::span<const uint8_t> r16_pak, RenderUploadMode upload_mode)
{
    VkDescriptorSet descriptor_set{};
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_main_descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &m_shadow_set_layout;
    GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &descriptor_set));
    // no mipmaps: a shadow map is only ever read at full resolution
    return RenderShadowMap(m_device.getHandle(), createTextureFromPak(r16_pak, VK_FORMAT_R16_UNORM, 2, false, upload_mode),
                           GPUDescriptorSet(m_delete_queue, m_main_descriptor_pool, descriptor_set));
}

RenderTexture RenderBackend::createTextureFromPak(std::span<const uint8_t> pak, VkFormat image_format, uint32_t bytes_per_texel, bool generate_mips,
                                                  RenderUploadMode upload_mode)
{
    ZoneScoped;

    GC_ASSERT(pak.size() > 2ULL * sizeof(uint32_t));
    uint32_t width{}, height{};
    std::memcpy(&width, pak.data(), sizeof(uint32_t));
    std::memcpy(&height, pak.data() + sizeof(uint32_t), sizeof(uint32_t));
    GC_ASSERT(width != 0 && height != 0);
    GC_ASSERT(pak.size() == 2 * sizeof(uint32_t) + (static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bytes_per_texel)));

    GC_TRACE("creating texture with size: {}x{}", width, height);

    const uint32_t mip_levels = generate_mips ? getMipLevels(width, height) : 1;

    // (the uploader makes the image, as allocating its memory takes a while too)
    return RenderTexture(m_uploader->uploadImage(image_format, width, height, mip_levels, false, {pak.subspan(2 * sizeof(uint32_t))}, upload_mode));
};

RenderTexture RenderBackend::createCubeTexture(std::array<std::span<const uint8_t>, 6> r8g8b8a8_paks, bool srgb, RenderUploadMode upload_mode)
{
    ZoneScoped;

    uint32_t width = 0, height = 0;

    for (const auto& face_pak : r8g8b8a8_paks) {
        GC_ASSERT(face_pak.size() > 2ULL * sizeof(uint32_t));
        uint32_t face_width{}, face_height{};
        std::memcpy(&face_width, face_pak.data(), sizeof(uint32_t));
        std::memcpy(&face_height, face_pak.data() + sizeof(uint32_t), sizeof(uint32_t));
        GC_ASSERT(face_width != 0 && face_height != 0);

        if (width == 0) {
            width = face_width;
            height = face_height;
        }

        GC_ASSERT(face_width == width);
        GC_ASSERT(face_height == height);
    }

    // the 6 faces go into the staging buffer one after another
    RenderUploader::Sources faces{};
    for (const auto& face_pak : r8g8b8a8_paks) {
        GC_ASSERT(face_pak.size() == 2 * sizeof(uint32_t) + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL));
        faces.push_back(face_pak.subspan(2 * sizeof(uint32_t)));
    }

    const uint32_t mip_levels = 1; // mipmaps aren't generated for cube maps yet

    const VkFormat image_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return RenderTexture(m_uploader->uploadImage(image_format, width, height, mip_levels, true, faces, upload_mode));
}

RenderTexture RenderBackend::createCubeMap(std::span<const uint8_t> cube_pak, RenderUploadMode upload_mode, uint32_t* mip_levels_out)
{
    const std::optional<ResourceCubeMap> cube_map = ResourceCubeMap::create(cube_pak);
    if (!cube_map) {
        abortGame("createCubeMap() called with data that isn't a cube map");
    }
    if (mip_levels_out) {
        *mip_levels_out = cube_map->info.mip_count;
    }
    return RenderTexture(m_uploader->uploadImageLevels(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, sizeof(uint32_t), cube_map->info.face_size,
                                                       cube_map->info.face_size, cube_map->info.mip_count, true,
                                                       {cube_pak.subspan(ResourceCubeMap::HEADER_SIZE)}, upload_mode));
}

RenderEnvironment RenderBackend::createEnvironment(std::span<const uint8_t> sky_pak, std::span<const uint8_t> irradiance_pak,
                                                   std::span<const uint8_t> specular_pak, RenderUploadMode upload_mode)
{
    VkDescriptorSet descriptor_set{};
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_main_descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &m_environment_set_layout;
    GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &descriptor_set));

    // (the small ones first, so that the sky doesn't hold them up)
    uint32_t specular_level_count{};
    RenderTexture irradiance = createCubeMap(irradiance_pak, upload_mode);
    RenderTexture specular = createCubeMap(specular_pak, upload_mode, &specular_level_count);
    RenderTexture sky = createCubeMap(sky_pak, upload_mode);
    return RenderEnvironment(m_device.getHandle(), std::move(sky), std::move(irradiance), std::move(specular), specular_level_count,
                             GPUDescriptorSet(m_delete_queue, m_main_descriptor_pool, descriptor_set));
}

RenderMesh RenderBackend::createMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices, RenderUploadMode upload_mode)
{
    GC_ASSERT(vertices.size() <= static_cast<size_t>(std::numeric_limits<decltype(indices)::value_type>::max()));
    GC_ASSERT(indices.size() <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));

    const uint32_t num_indices = static_cast<uint32_t>(indices.size());
    const size_t vertices_size = vertices.size() * sizeof(decltype(vertices)::value_type);
    const size_t indices_size = indices.size() * sizeof(decltype(indices)::value_type);
    const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(vertices_size + indices_size);

    // One buffer holds the vertices, then the indices
    VkBuffer buffer{};
    VmaAllocation buffer_alloc{};
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo buffer_alloc_info{};
        buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_alloc_info.flags = 0;
        buffer_alloc_info.priority = 0.5f;
        GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc, nullptr));
    }
    GPUBuffer managed_buffer(m_delete_queue, buffer, buffer_alloc);

    const std::span<const uint8_t> vertex_bytes(reinterpret_cast<const uint8_t*>(vertices.data()), vertices_size);
    const std::span<const uint8_t> index_bytes(reinterpret_cast<const uint8_t*>(indices.data()), indices_size);
    RenderUpload upload = m_uploader->uploadBuffer(buffer, {vertex_bytes, index_bytes}, upload_mode);

    return RenderMesh(std::move(managed_buffer), std::move(upload), static_cast<VkDeviceSize>(vertices_size), VK_INDEX_TYPE_UINT16, num_indices);
}

RenderTerrain RenderBackend::createTerrain(uint32_t tile_quads, uint32_t slot_count)
{
    GC_ASSERT(tile_quads >= 4 && (tile_quads & (tile_quads - 1)) == 0);

    // All of the slots are in one storage buffer, which can't be bigger than the GPU allows
    const VkDeviceSize slot_size = static_cast<VkDeviceSize>(tile_quads + 1) * (tile_quads + 1) * sizeof(uint32_t);
    const VkDeviceSize max_range = m_device.getProperties().props.properties.limits.maxStorageBufferRange;
    if (slot_count * slot_size > max_range) {
        const uint32_t fewer = static_cast<uint32_t>(max_range / slot_size);
        GC_WARN("A terrain's tile cache can't have {} slots, as the GPU's storage buffers can't be that big. It has {} instead", slot_count, fewer);
        slot_count = fewer;
    }
    GC_ASSERT(slot_count > 0);

    VkBuffer tiles{};
    VmaAllocation tiles_allocation{};
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = slot_count * slot_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc_info.priority = 0.5f;
        GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &buffer_info, &alloc_info, &tiles, &tiles_allocation, nullptr));
    }
    GPUBuffer tiles_buffer(m_delete_queue, tiles, tiles_allocation);

    const std::vector<uint32_t> indices = RenderTerrain::makePatchIndices(tile_quads);
    VkBuffer index_buffer{};
    VmaAllocation index_allocation{};
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = indices.size() * sizeof(uint32_t);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.priority = 0.5f;
        GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &buffer_info, &alloc_info, &index_buffer, &index_allocation, nullptr));
    }
    GPUBuffer indices_buffer(m_delete_queue, index_buffer, index_allocation);
    RenderUploader::Sources sources{};
    sources.push_back(std::span(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(uint32_t)));
    RenderUpload indices_upload = m_uploader->uploadBuffer(index_buffer, sources, RenderUploadMode::IMMEDIATE);

    VkDescriptorSet descriptor_set{};
    {
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = m_main_descriptor_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &m_terrain_set_layout;
        GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &descriptor_set));

        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = tiles;
        buffer_info.offset = 0;
        buffer_info.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(m_device.getHandle(), 1, &write, 0, nullptr);
    }

    GC_DEBUG("Created a terrain tile cache of {} slots ({})", slot_count, bytesToHumanReadable(slot_count * slot_size));
    return RenderTerrain(*m_uploader, m_delete_queue, tile_quads, slot_count, std::move(tiles_buffer), std::move(indices_buffer), std::move(indices_upload),
                         static_cast<uint32_t>(indices.size()), GPUDescriptorSet(m_delete_queue, m_main_descriptor_pool, descriptor_set));
}

// textures passed as parameters must outlive the material!
RenderMaterial RenderBackend::createMaterial(RenderTexture& base_color, RenderTexture& orm, RenderTexture& normal, RenderTexture& emissive,
                                             MaterialFeatures features, MaterialBlendMode blend_mode, const MaterialConstants& constants)
{
    VkDescriptorSet descriptor_set{};
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_main_descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &m_material_set_layout;
    GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &descriptor_set));
    return RenderMaterial(m_device.getHandle(), GPUDescriptorSet(m_delete_queue, m_main_descriptor_pool, descriptor_set), base_color, orm, normal, emissive,
                          features, blend_mode, constants);
}

void RenderBackend::waitIdle()
{
    /* ensure GPU is not using any command buffers etc. */
    GC_CHECKVK(vkDeviceWaitIdle(m_device.getHandle()));
}

void RenderBackend::recreateRenderImages()
{
    if (m_framebuffer_image) {
        vkDestroyImageView(m_device.getHandle(), m_framebuffer_image_view, nullptr);
        vmaDestroyImage(m_allocator.getHandle(), m_framebuffer_image, m_framebuffer_image_allocation);
        vkDestroyImageView(m_device.getHandle(), m_color_attachment_image_view, nullptr);
        vmaDestroyImage(m_allocator.getHandle(), m_color_attachment_image, m_color_attachment_allocation);
        vkDestroyImageView(m_device.getHandle(), m_depth_stencil_attachment_view, nullptr);
        vmaDestroyImage(m_allocator.getHandle(), m_depth_stencil_attachment_image, m_depth_stencil_attachment_allocation);
    }

    const VkFormat swapchain_format = m_swapchain.getSurfaceFormat().format;
    const auto [width, height] = m_swapchain.getExtent();

    std::tie(m_color_attachment_image, m_color_attachment_allocation) =
        vkutils::createImage(m_allocator.getHandle(), swapchain_format, width, height, 1, m_msaa_samples,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, 1.0f, true);
    m_color_attachment_image_view = vkutils::createImageView(m_device.getHandle(), m_color_attachment_image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT, 1);

    std::tie(m_depth_stencil_attachment_image, m_depth_stencil_attachment_allocation) =
        vkutils::createImage(m_allocator.getHandle(), m_depth_stencil_attachment_format, width, height, 1, m_msaa_samples,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, 1.0f, true);
    m_depth_stencil_attachment_view = vkutils::createImageView(m_device.getHandle(), m_depth_stencil_attachment_image, m_depth_stencil_attachment_format,
                                                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1);

    std::tie(m_framebuffer_image, m_framebuffer_image_allocation) =
        vkutils::createImage(m_allocator.getHandle(), swapchain_format, width, height, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1.0f, true);
    m_framebuffer_image_view = vkutils::createImageView(m_device.getHandle(), m_framebuffer_image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
}

void RenderBackend::recreateFramesInFlightResources()
{
    // wait for any work on the queue used for rendering and presentation to finish.
    GC_CHECKVK(vkQueueWaitIdle(m_device.getMainQueue()));

    for (const auto& stuff : m_fif) {
        vkDestroyCommandPool(m_device.getHandle(), stuff.pool, nullptr);
    }

    m_fif.resize(m_requested_frames_in_flight);

    /* Create 1 command buffer per frame in flight */
    for (auto& stuff : m_fif) {
        {
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            info.queueFamilyIndex = m_device.getQueueFamilyIndex();
            GC_CHECKVK(vkCreateCommandPool(m_device.getHandle(), &info, nullptr, &stuff.pool));
        }
        {
            VkCommandBufferAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandBufferCount = 1;
            info.commandPool = stuff.pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &info, &stuff.cmd));
        }
        stuff.command_buffer_available_value = 0;
    }

    // RenderBuffers rely on the number of frames in flight

    constexpr VkDeviceSize FRAME_UNIFORM_BUFFER_SIZE = sizeof(FrameUniformData);
    m_frame_uniform_buffer = std::make_unique<RenderBuffer>(m_delete_queue, m_allocator.getHandle(), static_cast<uint32_t>(m_fif.size()),
                                                            FRAME_UNIFORM_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    constexpr VkDeviceSize INSTANCING_BUFFER_INITIAL_SIZE = sizeof(glm::mat4) * 100; // 100 instances
    m_instancing_transforms_buffer = std::make_unique<RenderBuffer>(m_delete_queue, m_allocator.getHandle(), static_cast<uint32_t>(m_fif.size()),
                                                                    INSTANCING_BUFFER_INITIAL_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    constexpr VkDeviceSize TERRAIN_PATCHES_BUFFER_INITIAL_SIZE = sizeof(TerrainPatch) * 1024;
    m_terrain_patches_buffer = std::make_unique<RenderBuffer>(m_delete_queue, m_allocator.getHandle(), static_cast<uint32_t>(m_fif.size()),
                                                              TERRAIN_PATCHES_BUFFER_INITIAL_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    constexpr VkDeviceSize UI_VERTEX_BUFFER_INITIAL_SIZE = sizeof(UIVertex) * 16384;
    m_ui_vertex_buffer = std::make_unique<RenderBuffer>(m_delete_queue, m_allocator.getHandle(), static_cast<uint32_t>(m_fif.size()),
                                                        UI_VERTEX_BUFFER_INITIAL_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    constexpr VkDeviceSize UI_INDEX_BUFFER_INITIAL_SIZE = sizeof(uint32_t) * 32768;
    m_ui_index_buffer = std::make_unique<RenderBuffer>(m_delete_queue, m_allocator.getHandle(), static_cast<uint32_t>(m_fif.size()),
                                                       UI_INDEX_BUFFER_INITIAL_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    {
        VkDescriptorSet ds{};
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = m_main_descriptor_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &m_frame_set_layout;
        GC_CHECKVK(vkAllocateDescriptorSets(m_device.getHandle(), &info, &ds));

        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = m_frame_uniform_buffer->getBuffer();
        buf_info.offset = 0;
        buf_info.range = FRAME_UNIFORM_BUFFER_SIZE;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = ds;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buf_info;

        vkUpdateDescriptorSets(m_device.getHandle(), 1, &write, 0, nullptr);

        m_frame_uniform_buffer_set = std::make_unique<GPUDescriptorSet>(m_delete_queue, m_main_descriptor_pool, ds);
    }
}

void RenderBackend::waitForFrameReady()
{
    ZoneScopedC(tracy::Color::Crimson);

    const auto& stuff = m_fif[m_frame_count % m_fif.size()];

    ZoneValue(stuff.command_buffer_available_value);

    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &m_main_timeline_semaphore;
    info.pValues = &stuff.command_buffer_available_value;
    GC_CHECKVK(vkWaitSemaphores(m_device.getHandle(), &info, UINT64_MAX));
}

} // namespace gc
