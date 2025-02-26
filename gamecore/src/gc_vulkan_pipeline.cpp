#include "gamecore/gc_vulkan_pipeline.h"

#include "gamecore/gc_app.h"
#include "gamecore/gc_vulkan_renderer.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_compile_shader.h"

namespace gc {

std::pair<VkPipeline, VkPipelineLayout> createPipeline()
{
    VkDevice device = app().vulkanRenderer().getDevice().getDevice();

    static const std::string vertex_src{
        "\
        #version 450\n\
        const vec2 vertices[] = {vec2(0.0, 0.5), vec2(-0.4330127018922193, -0.25), vec2(0.4330127018922193, -0.25)};\n\
        const vec3 colors[] = {vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0)};\n\
        layout(push_constant) uniform Constants { vec2 pos; } constants;\n\
        layout(location = 0) out vec3 color;\n\
        void main() {\n\
            gl_Position = vec4(vertices[gl_VertexIndex], 0.0, 1.0);\n\
            gl_Position.xy += constants.pos;\n\
            color = colors[gl_VertexIndex];\n\
            color.r *= (cos(constants.pos.x) + 1.0) * 0.5;\n\
            color.g *= (sin(constants.pos.y) + 1.0) * 0.5;\n\
            color.b *= (sin(constants.pos.x * 2.0) + 1.0) * 0.5;\n\
            gl_Position.y *= -1.0;\n\
        }\n\
        "};

    static const std::string fragment_src{
        "\
        #version 450\n\
        layout(location = 0) in vec3 color;\n\
        layout(location = 0) out vec4 outColor;\n\
        void main() {\n\
        outColor = vec4(color, 1.0);\n\
        }\n\
        "};

    const auto vertex_spv = compileShaderModule(vertex_src, ShaderModuleType::VERTEX);
    const auto fragment_spv = compileShaderModule(fragment_src, ShaderModuleType::FRAGMENT);
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
    push_const_range.size = sizeof(float) * 2; // vec2
    push_const_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pNext = nullptr;
    layout_info.flags = 0;
    layout_info.setLayoutCount = 0;
    layout_info.pSetLayouts = nullptr;
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
    rendering_info.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    rendering_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

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
    info.pDepthStencilState = nullptr; // no depth stencil attachments used during rendering
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
    VkDevice device = app().vulkanRenderer().getDevice().getDevice();
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
}


}