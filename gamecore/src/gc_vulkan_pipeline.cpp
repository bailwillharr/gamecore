#include "gamecore/gc_vulkan_pipeline.h"

#include <fstream>
#include <memory>
#include <vector>

#include "gamecore/gc_app.h"
#include "gamecore/gc_vulkan_renderer.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_compile_shader.h"
#include "gamecore/gc_disk_io.h"

namespace gc {

static std::vector<char> readTextFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate);
    if (file.is_open() == false) {
        abortGame("Unable to open file: {}", path);
    }

    std::vector<char> buffer(static_cast<std::size_t>(file.tellg()) + 1);

    file.seekg(0);

    int i = 0;
    while (!file.eof()) {
        char c{};
        file.read(&c, 1); // reading 1 char at a time

        buffer.data()[i] = c;

        ++i;
    }

    // append zero byte
    buffer.data()[buffer.size() - 1] = '\0';

    file.close();

    return buffer;
}

std::pair<VkPipeline, VkPipelineLayout> createPipeline(VkDescriptorSetLayout set_layout)
{
    VkDevice device = app().vulkanRenderer().getHandle().getHandle();

    const auto vertex_src = readTextFile(std::filesystem::path(findContentDir().value() / "cube.vert").string());
    const std::string vertex_src_string(vertex_src.data());

    const auto fragment_src = readTextFile(std::filesystem::path(findContentDir().value() / "cube.frag").string());
    const std::string fragment_src_string(fragment_src.data());

    const auto vertex_spv = compileShaderModule(vertex_src_string, ShaderModuleType::VERTEX);
    const auto fragment_spv = compileShaderModule(fragment_src_string, ShaderModuleType::FRAGMENT);
    if (vertex_spv.empty() || fragment_spv.empty()) {
        abortGame("Shader compile failed");
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;
    module_info.codeSize = vertex_spv.size() * sizeof(decltype(vertex_spv)::value_type);
    module_info.pCode = vertex_spv.data();
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &vertex_module));
    module_info.codeSize = fragment_spv.size() * sizeof(decltype(fragment_spv)::value_type);
    module_info.pCode = fragment_spv.data();
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreateShaderModule(device, &module_info, nullptr, &fragment_module));

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].flags = 0;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = nullptr;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].flags = 0;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = nullptr;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.pNext = nullptr;
    vertex_input.flags = 0;
    vertex_input.vertexBindingDescriptionCount = 0;
    vertex_input.pVertexBindingDescriptions = nullptr;
    vertex_input.vertexAttributeDescriptionCount = 0;
    vertex_input.pVertexAttributeDescriptions = nullptr;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.pNext = nullptr;
    input_assembly.flags = 0;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.pNext = nullptr;
    viewport.flags = 0;
    viewport.viewportCount = 1;
    viewport.pViewports = nullptr; // dynamic state
    viewport.scissorCount = 1;
    viewport.pScissors = nullptr;  // dynamic state

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE; // enabling this will not run the fragment shaders at all
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.lineWidth = 1.0f;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.depthBiasConstantFactor = 0.0f; // ignored
    rasterization.depthBiasClamp = 0.0f;          // ignored
    rasterization.depthBiasSlopeFactor = 0.0f;    // ignored

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.minSampleShading = 1.0f;          // ignored
    multisampling.pSampleMask = nullptr;            // ignored
    multisampling.alphaToCoverageEnable = VK_FALSE; // ignored
    multisampling.alphaToOneEnable = VK_FALSE;      // ignored

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.blendEnable = VK_FALSE;
    // other values ignored when blendEnable is false
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.pNext = nullptr;
    color_blend.flags = 0;
    color_blend.logicOpEnable = VK_FALSE;
    color_blend.logicOp = VK_LOGIC_OP_CLEAR; // ignored
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &color_blend_attachment;
    color_blend.blendConstants[0] = 0.0f;
    color_blend.blendConstants[1] = 0.0f;
    color_blend.blendConstants[2] = 0.0f;
    color_blend.blendConstants[3] = 0.0f;

    auto dynamic_states = std::to_array({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});

    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.pNext = nullptr;
    dynamic.flags = 0;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkPushConstantRange push_const_range{};
    push_const_range.offset = 0;
    push_const_range.size = sizeof(float) * 16 * 2; // mat4 * 2
    push_const_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pNext = nullptr;
    layout_info.flags = 0;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_const_range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    GC_CHECKVK(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));

    VkFormat color_attachment_format = app().vulkanRenderer().getSwapchain().getSurfaceFormat().format;
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_attachment_format;
    rendering_info.depthAttachmentFormat = app().vulkanRenderer().getDepthStencilFormat();
    rendering_info.stencilAttachmentFormat = rendering_info.depthAttachmentFormat;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.pNext = nullptr;
    depth_stencil.flags = 0;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;
    // depth_stencil.front = ;
    // depth_stencil.back = ;
    depth_stencil.minDepthBounds = 0.0f;
    depth_stencil.maxDepthBounds = 1.0f;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering_info;
    info.flags = 0;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pTessellationState = nullptr; // no tesselation shaders used
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = 0;
    VkPipeline pipeline{};
    GC_CHECKVK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));

    vkDestroyShaderModule(device, fragment_module, nullptr);
    vkDestroyShaderModule(device, vertex_module, nullptr);

    return std::make_pair(pipeline, layout);
}

void destroyPipeline(VkPipeline pipeline, VkPipelineLayout layout)
{
    VkDevice device = app().vulkanRenderer().getHandle().getHandle();
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
}

} // namespace gc
