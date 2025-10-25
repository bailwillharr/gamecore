#include "gamecore/gc_render_backend.h"

#include <cstring>

#include <array>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_timer.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <backends/imgui_impl_vulkan.h>

#include "gamecore/gc_app.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_render_world.h"
#include "gamecore/gc_vulkan_utils.h"

namespace gc {

static uint32_t getMipLevels(uint32_t width, uint32_t height) { return static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1; }

// The original image (mip level 0) should be TRANSFER_SRC image layout, the rest should be TRANSFER_DST
// At the end, will transition all mip levels to SHADER_READ_ONLY_OPTIMAL
static void generateMipMaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height)
{
    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[0] = {0, 0, 0};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[1] = {0, 0, 0};

    std::array<VkImageMemoryBarrier2, 2> barriers{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[1] = barriers[0];
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();

    uint32_t level_width = width;
    uint32_t level_height = height;
    uint32_t mip_dst_index = 0;
    while (level_width > 1 || level_height > 1) {
        const uint32_t src_width = level_width;
        const uint32_t src_height = level_height;
        level_width = (level_width > 1) ? level_width / 2 : 1;
        level_height = (level_height > 1) ? level_height / 2 : 1;
        ++mip_dst_index;
        const uint32_t mip_src_index = mip_dst_index - 1;

        // blit mip_src_index to mip_dst_index
        region.srcSubresource.mipLevel = mip_src_index;
        region.srcOffsets[1] = {static_cast<int>(src_width), static_cast<int>(src_height), 1};
        region.dstSubresource.mipLevel = mip_dst_index;
        region.dstOffsets[1] = {static_cast<int>(level_width), static_cast<int>(level_height), 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

        // barrier for mip_src_index TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        barriers[0].subresourceRange.baseMipLevel = mip_src_index;
        // barrier for mip_dst_index TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
        barriers[1].subresourceRange.baseMipLevel = mip_dst_index;
        if (level_width == 1 && level_height == 1) {
            // last mip level won't be blitted from so make it SHADER_READ_ONLY_OPTIMAL
            barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier2(cmd, &dep);
    }
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

[[maybe_unused]] static void wasteGPUCycles(VkImage image, VkDevice device, VkCommandBuffer cmd, size_t iters)
{
    (void)image;
    static VkPipeline pipeline{};
    static VkPipelineLayout layout{};
    if (!pipeline) {
        VkShaderModule shader_module{};
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        auto code = App::instance().content().findAsset(Name("busy.comp"), gcpak::GcpakAssetType::SPIRV_SHADER);
        if (code.size() == 0) {
            abortGame("Couldn't find compute shader binary");
        }
        module_info.codeSize = code.size();
        module_info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &shader_module));
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 0;
        layout_info.pSetLayouts = nullptr;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;
        GC_CHECKVK(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.flags = 0;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader_module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = nullptr;
        info.layout = layout;
        GC_CHECKVK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        vkDestroyShaderModule(device, shader_module, nullptr);
    }

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdDispatch(cmd, (static_cast<uint32_t>(iters) + 64 - 1) / 64, 16, 1024);

    vkCmdPipelineBarrier2(cmd, &dep);
#if 0
    (void)device;

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    for (size_t i = 0; i < iters; ++i) {
        vkCmdPipelineBarrier2(cmd, &dep);
        VkClearColorValue color{};
        color.float32[0] = 1.0f;
        color.float32[1] = 1.0f;
        color.float32[2] = 1.0f;
        color.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        vkCmdPipelineBarrier2(cmd, &dep);
    }
#endif
}

static GPUPipeline createSkyboxPipeline(VkDevice device, GPUResourceDeleteQueue& delete_queue, VkPipelineLayout pipeline_layout,
                                        VkFormat color_attachment_format, VkFormat depth_stencil_attachment_format,
                                        VkSampleCountFlagBits color_attachment_sample_count)
{
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;

    auto vertex_spv = App::instance().content().findAsset(Name("skybox.vert"), gcpak::GcpakAssetType::SPIRV_SHADER);
    auto fragment_spv = App::instance().content().findAsset(Name("skybox.frag"), gcpak::GcpakAssetType::SPIRV_SHADER);

    module_info.codeSize = vertex_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(vertex_spv.data());
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &vertex_module));

    module_info.codeSize = fragment_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(fragment_spv.data());
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &fragment_module));

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

    VkPipelineVertexInputStateCreateInfo vertex_input_state{};
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.pNext = nullptr;
    vertex_input_state.flags = 0;
    vertex_input_state.vertexBindingDescriptionCount = 0;
    vertex_input_state.pVertexBindingDescriptions = nullptr;
    vertex_input_state.vertexAttributeDescriptionCount = 0;
    vertex_input_state.pVertexAttributeDescriptions = nullptr;

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
    rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization_state.depthBiasEnable = VK_FALSE;
    rasterization_state.depthBiasConstantFactor = 0.0f; // ignored
    rasterization_state.depthBiasClamp = 0.0f;          // ignored
    rasterization_state.depthBiasSlopeFactor = 0.0f;    // ignored

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = depth_stencil_attachment_format;
    rendering_info.stencilAttachmentFormat = depth_stencil_attachment_format;

    VkPipelineMultisampleStateCreateInfo multisample_state{};
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = color_attachment_sample_count;
    multisample_state.sampleShadingEnable = VK_FALSE;
    multisample_state.minSampleShading = 1.0f;          // ignored
    multisample_state.pSampleMask = nullptr;            // ignored
    multisample_state.alphaToCoverageEnable = VK_FALSE; // ignored
    multisample_state.alphaToOneEnable = VK_FALSE;      // ignored

    VkPipelineDepthStencilStateCreateInfo depth_stencil_state{};
    depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_FALSE; // it will always be at 1.0 (max depth) so no point writing to the depth buffer
    depth_stencil_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;
    depth_stencil_state.stencilTestEnable = VK_FALSE;
    depth_stencil_state.front = {};
    depth_stencil_state.back = {};

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blend_state{};
    color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.logicOpEnable = VK_FALSE;
    color_blend_state.attachmentCount = 1;
    color_blend_state.pAttachments = &color_blend_attachment;

    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

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
    info.layout = pipeline_layout;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = -1;

    VkPipeline pipeline{};
    GC_CHECKVK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));

    vkDestroyShaderModule(device, fragment_module, nullptr);
    vkDestroyShaderModule(device, vertex_module, nullptr);

    return GPUPipeline(delete_queue, pipeline);
}

RenderBackend::RenderBackend(SDL_Window* window_handle)
    : m_device(), m_allocator(m_device), m_swapchain(m_device, window_handle), m_delete_queue(m_device.getHandle(), m_allocator.getHandle())
{
    // create sampler
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.mipLodBias = 0.0f;
        info.anisotropyEnable = VK_FALSE;
        // info.maxAnisotropy = 16.0f;
        info.compareEnable = VK_FALSE;
        info.minLod = 0.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        GC_CHECKVK(vkCreateSampler(m_device.getHandle(), &info, nullptr, &m_sampler));
    }

    // create main descriptor pool for long-lasting static resources
    {
        std::array<VkDescriptorPoolSize, 1> pool_sizes = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100},
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

    // create descriptor set layout
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
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
        GC_CHECKVK(vkCreateDescriptorSetLayout(m_device.getHandle(), &info, nullptr, &m_descriptor_set_layout));
    }

    // create a simple pipeline layout for all 3D stuff (for now)
    {
        VkPushConstantRange push_constant_range{};
        push_constant_range.offset = 0;
        // push_constant_range.size = 128; // Guaranteed minimum size https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#limits-minmax
        push_constant_range.size = 64 + 64 + 64 + 16; // mat4, mat4, mat4, vec3
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &m_descriptor_set_layout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push_constant_range;
        GC_CHECKVK(vkCreatePipelineLayout(m_device.getHandle(), &info, nullptr, &m_pipeline_layout));
    }

    // find depth stencil format to use
    {
        VkFormatProperties depth_format_props{};
        vkGetPhysicalDeviceFormatProperties(m_device.getPhysicalDevice(), VK_FORMAT_D24_UNORM_S8_UINT,
                                            &depth_format_props); // NVIDIA Best practices recommend this
        if (depth_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depth_stencil_attachment_format = VK_FORMAT_D24_UNORM_S8_UINT;
        }
        else {
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
    m_transfer_timeline_value = 0;
    m_framebuffer_copy_finished_value = 0;

    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = m_device.getQueueFamilyIndex();
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        GC_CHECKVK(vkCreateCommandPool(m_device.getHandle(), &info, nullptr, &m_transfer_cmd_pool));
    }

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

    // m_main_timeline_semaphore and the frame in flight command pools will be created when submitFrame() is called for the first time.

    GC_TRACE("Initialised RenderBackend");
}

RenderBackend::~RenderBackend()
{
    GC_TRACE("Destroying RenderBackend...");

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

    vkDestroyCommandPool(m_device.getHandle(), m_transfer_cmd_pool, nullptr);

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

    vkDestroyPipelineLayout(m_device.getHandle(), m_pipeline_layout, nullptr);

    vkDestroyDescriptorSetLayout(m_device.getHandle(), m_descriptor_set_layout, nullptr);
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

void RenderBackend::submitFrame(bool window_resized, const WorldDrawData& world_draw_data)
{
    ZoneScoped;

    if (m_requested_frames_in_flight != static_cast<int>(m_fif.size())) {
        recreateFramesInFlightResources();
    }

    auto& stuff = m_fif[m_frame_count % m_fif.size()];

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
        color_attachment.clearValue.color.float32[0] = 0.0f;
        color_attachment.clearValue.color.float32[1] = 0.0f;
        color_attachment.clearValue.color.float32[2] = 0.0f;
        color_attachment.clearValue.color.float32[3] = 0.0f;
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = m_depth_stencil_attachment_view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 1.0f;
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
        info.pStencilAttachment = nullptr;
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

        recordWorldRenderingCommands(stuff.cmd, m_pipeline_layout, m_main_timeline_semaphore, m_main_timeline_value + 1, world_draw_data);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), stuff.cmd);

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

GPUPipeline RenderBackend::createPipeline(std::span<const uint8_t> vertex_spv, std::span<const uint8_t> fragment_spv)
{
    ZoneScoped;

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;

    module_info.codeSize = vertex_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(vertex_spv.data());
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(m_device.getHandle(), &module_info, nullptr, &vertex_module));

    module_info.codeSize = fragment_spv.size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(fragment_spv.data());
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(m_device.getHandle(), &module_info, nullptr, &fragment_module));

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
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_state.minDepthBounds = 0.0f;
    depth_stencil_state.maxDepthBounds = 1.0f;
    depth_stencil_state.stencilTestEnable = VK_FALSE;
    depth_stencil_state.front = {};
    depth_stencil_state.back = {};

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

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
    info.layout = m_pipeline_layout;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = -1;

    VkPipeline handle{};
    GC_CHECKVK(vkCreateGraphicsPipelines(m_device.getHandle(), VK_NULL_HANDLE, 1, &info, nullptr, &handle));

    vkDestroyShaderModule(m_device.getHandle(), fragment_module, nullptr);
    vkDestroyShaderModule(m_device.getHandle(), vertex_module, nullptr);

    return GPUPipeline(m_delete_queue, handle);
}

GPUPipeline RenderBackend::createSkyboxPipeline()
{
    return gc::createSkyboxPipeline(m_device.getHandle(), m_delete_queue, m_pipeline_layout, m_swapchain.getSurfaceFormat().format,
                                    m_depth_stencil_attachment_format, m_msaa_samples);
}

RenderTexture RenderBackend::createTexture(std::span<const uint8_t> r8g8b8a8_pak, bool srgb)
{
    ZoneScoped;

    GC_ASSERT(r8g8b8a8_pak.size() > 2ULL * sizeof(uint32_t));
    uint32_t width{}, height{};
    std::memcpy(&width, r8g8b8a8_pak.data(), sizeof(uint32_t));
    std::memcpy(&height, r8g8b8a8_pak.data() + sizeof(uint32_t), sizeof(uint32_t));
    GC_ASSERT(width != 0 && height != 0);
    GC_ASSERT(r8g8b8a8_pak.size() == 2 * sizeof(uint32_t) + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL));
    const uint8_t* const bitmap_data_start = r8g8b8a8_pak.data() + 2 * sizeof(uint32_t);

    GC_TRACE("creating texture with size: {}x{}", width, height);

    const uint32_t mip_levels = getMipLevels(width, height);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4ULL;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo buffer_alloc_info{};
    buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    buffer_alloc_info.priority = 0.5f;
    VkBuffer buffer{};
    VmaAllocation buffer_alloc{};
    GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc, nullptr));

    uint8_t* data_dest{};
    GC_CHECKVK(vmaMapMemory(m_allocator.getHandle(), buffer_alloc, reinterpret_cast<void**>(&data_dest)));
    std::memcpy(data_dest, bitmap_data_start, buffer_info.size);
    vmaUnmapMemory(m_allocator.getHandle(), buffer_alloc);

    GPUBuffer gpu_staging_buffer(m_delete_queue, buffer, buffer_alloc);

    const VkFormat image_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    auto [image, allocation] = vkutils::createImage(m_allocator.getHandle(), image_format, width, height, mip_levels, VK_SAMPLE_COUNT_1_BIT,
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0.5f);

    {
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandPool = m_transfer_cmd_pool;
        cmd_info.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &cmd_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        if (width > 16) {
            // wasteGPUCycles(image, m_device.getHandle(), cmd, 20'000'000'000LL / (width * height));
        }

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.dstQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = 0;
        region.imageOffset.y = 0;
        region.imageOffset.z = 0;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, gpu_staging_buffer.getHandle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mip_levels > 1) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier2(cmd, &dependency);
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.subresourceRange.baseMipLevel = 1;
            barrier.subresourceRange.levelCount = mip_levels - 1;
            vkCmdPipelineBarrier2(cmd, &dependency);

            generateMipMaps(cmd, image, width, height);
        }
        else {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        GC_CHECKVK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = cmd;
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_transfer_timeline_semaphore;
        signal_info.value = ++m_transfer_timeline_value;
        if (mip_levels > 1) {
            signal_info.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        }
        else {
            signal_info.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        }
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 0;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;
        const auto transfer_queue = m_device.getTransferQueue();
        GC_CHECKVK(vkQueueSubmit2(transfer_queue, 1, &submit_info, VK_NULL_HANDLE));

        GPUResourceDeleteQueue::DeletionEntry command_buffer_deletion_entry{};
        command_buffer_deletion_entry.timeline_semaphore = m_transfer_timeline_semaphore;
        command_buffer_deletion_entry.resource_free_signal_value = m_transfer_timeline_value;
        const VkCommandPool transfer_cmd_pool = m_transfer_cmd_pool;
        command_buffer_deletion_entry.deleter = [transfer_cmd_pool, cmd](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
            GC_TRACE("freeing command buffer: {}", reinterpret_cast<void*>(cmd));
            vkFreeCommandBuffers(device, transfer_cmd_pool, 1, &cmd);
        };
        m_delete_queue.markForDeletion(command_buffer_deletion_entry);
    }

    auto gpu_image = std::make_shared<GPUImage>(m_delete_queue, image, allocation);

    // The destructor for GPUStagingBuffer will be called when this function returns.
    // It will be actually destroyed only after the image has been uploaded.
    gpu_staging_buffer.useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);
    gpu_image->useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);

    auto image_view = vkutils::createImageView(m_device.getHandle(), image, image_format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);

    return RenderTexture(GPUImageView(m_delete_queue, image_view, gpu_image));
};

RenderTexture RenderBackend::createCubeTexture(std::array<std::span<const uint8_t>, 6> r8g8b8a8_paks, bool srgb)
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

    const auto getDataStart = [&](int index) -> const uint8_t* {
        GC_ASSERT(r8g8b8a8_paks[index].size() == 2 * sizeof(uint32_t) + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL));
        return r8g8b8a8_paks[index].data() + 2 * sizeof(uint32_t);
    };

    uint32_t mip_levels = 1; // getMipLevels(width, height);

    const size_t face_buffer_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4ULL;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = face_buffer_size * 6ULL; // 6 faces in contiguous memory
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo buffer_alloc_info{};
    buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    buffer_alloc_info.priority = 0.5f;
    VkBuffer buffer{};
    VmaAllocation buffer_alloc{};
    GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc, nullptr));

    uint8_t* data_dest{};
    GC_CHECKVK(vmaMapMemory(m_allocator.getHandle(), buffer_alloc, reinterpret_cast<void**>(&data_dest)));
    for (int i = 0; i < 6; ++i) {
        std::memcpy(data_dest + face_buffer_size * i, getDataStart(i), face_buffer_size);
    }
    vmaUnmapMemory(m_allocator.getHandle(), buffer_alloc);

    GPUBuffer gpu_staging_buffer(m_delete_queue, buffer, buffer_alloc);

    const VkFormat image_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    auto [image, allocation] =
        vkutils::createImage(m_allocator.getHandle(), image_format, width, height, mip_levels, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0.5f, false, true);

    {
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandPool = m_transfer_cmd_pool;
        cmd_info.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &cmd_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.dstQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 6;
        region.imageOffset.x = 0;
        region.imageOffset.y = 0;
        region.imageOffset.z = 0;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, gpu_staging_buffer.getHandle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mip_levels > 1) {
            abortGame("Not yet implemented");
        }
        else {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 6;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        GC_CHECKVK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = cmd;
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_transfer_timeline_semaphore;
        signal_info.value = ++m_transfer_timeline_value;
        if (mip_levels > 1) {
            signal_info.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        }
        else {
            signal_info.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        }
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 0;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;
        const auto transfer_queue = m_device.getTransferQueue();
        GC_CHECKVK(vkQueueSubmit2(transfer_queue, 1, &submit_info, VK_NULL_HANDLE));

        GPUResourceDeleteQueue::DeletionEntry command_buffer_deletion_entry{};
        command_buffer_deletion_entry.timeline_semaphore = m_transfer_timeline_semaphore;
        command_buffer_deletion_entry.resource_free_signal_value = m_transfer_timeline_value;
        const VkCommandPool transfer_cmd_pool = m_transfer_cmd_pool;
        command_buffer_deletion_entry.deleter = [transfer_cmd_pool, cmd](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
            GC_TRACE("freeing command buffer: {}", reinterpret_cast<void*>(cmd));
            vkFreeCommandBuffers(device, transfer_cmd_pool, 1, &cmd);
        };
        m_delete_queue.markForDeletion(command_buffer_deletion_entry);
    }

    auto gpu_image = std::make_shared<GPUImage>(m_delete_queue, image, allocation);

    // The destructor for GPUStagingBuffer will be called when this function returns.
    // It will be actually destroyed only after the image has been uploaded.
    gpu_staging_buffer.useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);
    gpu_image->useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);

    auto image_view = vkutils::createImageView(m_device.getHandle(), image, image_format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels, true);

    return RenderTexture(GPUImageView(m_delete_queue, image_view, gpu_image));
}

RenderMaterial RenderBackend::createMaterial(const std::shared_ptr<RenderTexture>& base_color_texture,
                                             const std::shared_ptr<RenderTexture>& occlusion_roughness_metallic_texture,
                                             const std::shared_ptr<RenderTexture>& normal_texture, const std::shared_ptr<GPUPipeline>& pipeline)
{
    return RenderMaterial(m_device.getHandle(), m_main_descriptor_pool, m_descriptor_set_layout, base_color_texture, occlusion_roughness_metallic_texture,
                          normal_texture, pipeline);
}
RenderMesh RenderBackend::createMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices)
{
    GC_ASSERT(vertices.size() <= static_cast<size_t>(std::numeric_limits<decltype(indices)::value_type>::max()));
    GC_ASSERT(indices.size() <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));

    const uint32_t num_indices = static_cast<uint32_t>(indices.size());
    const size_t vertices_size = vertices.size() * sizeof(decltype(vertices)::value_type);
    const size_t indices_size = indices.size() * sizeof(decltype(indices)::value_type);
    const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(vertices_size + indices_size);

    // create and map staging buffer
    VkBuffer staging_buffer{};
    VmaAllocation staging_alloc{};
    {
        VkBufferCreateInfo staging_buffer_info{};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging_buffer_info.size = buffer_size;
        staging_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo staging_alloc_info{};
        staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        staging_alloc_info.priority = 0.5f;
        GC_CHECKVK(vmaCreateBuffer(m_allocator.getHandle(), &staging_buffer_info, &staging_alloc_info, &staging_buffer, &staging_alloc, nullptr));
    }
    GPUBuffer managed_staging_buffer(m_delete_queue, staging_buffer, staging_alloc);
    uint8_t* data_dest{};
    GC_CHECKVK(vmaMapMemory(m_allocator.getHandle(), staging_alloc, reinterpret_cast<void**>(&data_dest)));
    std::memcpy(data_dest, reinterpret_cast<const uint8_t*>(vertices.data()), vertices_size);
    std::memcpy(data_dest + vertices_size, reinterpret_cast<const uint8_t*>(indices.data()), indices_size);
    vmaUnmapMemory(m_allocator.getHandle(), staging_alloc);

    // create destination buffer
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

    // copy vertices and indices to the buffer
    {
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandPool = m_transfer_cmd_pool;
        cmd_info.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        GC_CHECKVK(vkAllocateCommandBuffers(m_device.getHandle(), &cmd_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size = buffer_size;
        vkCmdCopyBuffer(cmd, staging_buffer, buffer, 1, &region);

        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.dstQueueFamilyIndex = m_device.getQueueFamilyIndex();
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = buffer_size;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        GC_CHECKVK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = cmd;
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_transfer_timeline_semaphore;
        signal_info.value = ++m_transfer_timeline_value;
        signal_info.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 0;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;
        const auto transfer_queue = m_device.getTransferQueue();
        GC_CHECKVK(vkQueueSubmit2(transfer_queue, 1, &submit_info, VK_NULL_HANDLE));

        GPUResourceDeleteQueue::DeletionEntry command_buffer_deletion_entry{};
        command_buffer_deletion_entry.timeline_semaphore = m_transfer_timeline_semaphore;
        command_buffer_deletion_entry.resource_free_signal_value = m_transfer_timeline_value;
        const VkCommandPool transfer_cmd_pool = m_transfer_cmd_pool;
        command_buffer_deletion_entry.deleter = [transfer_cmd_pool, cmd](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
            GC_TRACE("freeing command buffer: {}", reinterpret_cast<void*>(cmd));
            vkFreeCommandBuffers(device, transfer_cmd_pool, 1, &cmd);
        };
        m_delete_queue.markForDeletion(command_buffer_deletion_entry);
    }

    managed_staging_buffer.useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);
    GPUBuffer managed_buffer(m_delete_queue, buffer, buffer_alloc);
    managed_buffer.useResource(m_transfer_timeline_semaphore, m_transfer_timeline_value);

    return RenderMesh(std::move(managed_buffer), static_cast<VkDeviceSize>(vertices_size), VK_INDEX_TYPE_UINT16, num_indices);
}

RenderMesh RenderBackend::createMeshFromAsset(std::span<const uint8_t> asset)
{
    GC_ASSERT(asset.size() > 2);
    uint16_t vertex_count{};
    std::memcpy(&vertex_count, asset.data(), sizeof(uint16_t));

    const uint8_t* const vertices_location = asset.data() + sizeof(uint16_t);
    const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
    const size_t index_count = (asset.data() + asset.size() - indices_location) / sizeof(uint16_t);

    const std::span<const MeshVertex> vertices(reinterpret_cast<const MeshVertex*>(vertices_location), vertex_count);
    const std::span<const uint16_t> indices(reinterpret_cast<const uint16_t*>(indices_location), index_count);

    return createMesh(vertices, indices);
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
    GC_TRACE("Recreating frames in flight resources. FIF count {}", m_requested_frames_in_flight);

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
