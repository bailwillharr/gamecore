#include "gamecore/gc_compile_shader.h"

#include <shaderc/shaderc.hpp>

#include "gamecore/gc_logger.h"
#include <gamecore/gc_assert.h>

namespace gc {

std::vector<uint32_t> compileShaderModule(const std::string& source, ShaderModuleType type)
{
    constexpr const char* SHADER_FILE_NAME = "shader.glsl";

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetAutoBindUniforms(false);

    GC_ASSERT(type == ShaderModuleType::VERTEX || type == ShaderModuleType::FRAGMENT);
    shaderc_shader_kind shader_kind = shaderc_vertex_shader;
    switch (type) {
        case ShaderModuleType::VERTEX:
            shader_kind = shaderc_vertex_shader;
            break;
        case ShaderModuleType::FRAGMENT:
            shader_kind = shaderc_fragment_shader;
            break;
    }

    const shaderc::PreprocessedSourceCompilationResult preprocessed = compiler.PreprocessGlsl(source, shader_kind, SHADER_FILE_NAME, options);

    if (preprocessed.GetCompilationStatus() == shaderc_compilation_status_success) {
        
        const std::string preprocessed_string{preprocessed.cbegin(), preprocessed.cend()};
        shaderc::SpvCompilationResult compiled_shader = compiler.CompileGlslToSpv(preprocessed_string.c_str(), shader_kind, SHADER_FILE_NAME, options);

        if (compiled_shader.GetCompilationStatus() == shaderc_compilation_status_success) {
            const std::vector<uint32_t> shader_bytecode = {compiled_shader.cbegin(), compiled_shader.cend()};
            return shader_bytecode;
        }
        else {
            GC_ERROR("Failed to compile shader module: {}", compiled_shader.GetErrorMessage());
        }
    }
    else {
        GC_ERROR("Failed to preprocess shader module: {}", preprocessed.GetErrorMessage());
    }

    GC_ERROR("compileShaderModule() failed");
    return {};
}

} // namespace gc