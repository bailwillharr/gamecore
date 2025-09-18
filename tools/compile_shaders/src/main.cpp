#include "compile_shaders.h"

#include <cstring>

#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>

#include <shaderc/shaderc.hpp>

#include <gcpak/gcpak.h>

static void fooyou()
{
    std::vector<int> myvec(10, 55);
    std::vector<int> myvec2{10, 55};
}

static std::vector<uint8_t> compileShader(const shaderc::Compiler& compiler, const std::filesystem::path& path)
{
    const auto filename = path.filename().string();

    std::ifstream source_file{path};
    if (!source_file) {
        std::cerr << "Failed to open shader source: " << filename << "\n";
        return {};
    }
    std::ostringstream source{};
    source << source_file.rdbuf();
    if (!source_file) {
        std::cerr << "Failed to read shader source: " << filename << "\n";
    }

    shaderc_shader_kind kind{};
    if (path.extension() == std::string(".vert")) {
        kind = shaderc_vertex_shader;
    }
    else if (path.extension() == std::string(".frag")) {
        kind = shaderc_fragment_shader;
    }
    else {
        std::cerr << "Shader source has invalid extension: " << filename << "\n";
        return {};
    }

    shaderc::CompileOptions options{};
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetAutoBindUniforms(false);
    options.SetWarningsAsErrors();

    shaderc::SpvCompilationResult compiledShader = compiler.CompileGlslToSpv(source.str(), kind, filename.c_str(), options);

    if (compiledShader.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::cerr << "Compilation error for " << filename << ":\n";
        std::cerr << compiledShader.GetErrorMessage() << "\n";
        return {};
    }

    const size_t binary_size = (compiledShader.cend() - compiledShader.cbegin()) * sizeof(shaderc::SpvCompilationResult::element_type);
    auto binary = std::vector<uint8_t>(binary_size);
    std::memcpy(binary.data(), compiledShader.cbegin(), binary_size);
    return binary;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    std::error_code ec{};

    fooyou();

    const auto shader_dir = std::filesystem::path(COMPILE_SHADERS_SOURCE_DIRECTORY).parent_path().parent_path() / "content" / "shader_src";
    if (!std::filesystem::exists(shader_dir, ec) || !std::filesystem::is_directory(shader_dir, ec)) {
        std::cerr << "Failed to find content directory! error: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    // find all .vert and .frag files and compile them
    const shaderc::Compiler compiler{};
    if (!compiler.IsValid()) {
        std::cerr << "Failed to initialise shaderc compiler!\n";
        return EXIT_FAILURE;
    }
    gcpak::GcpakCreator gcpak_creator{};
    for (const auto& dir_entry : std::filesystem::directory_iterator(shader_dir)) {
        const auto extension = dir_entry.path().extension();
        if (dir_entry.is_regular_file() && (extension == std::string{".vert"} || extension == std::string{".frag"})) {
            auto binary = compileShader(compiler, dir_entry);
            if (binary.empty()) {
                std::cerr << "Failed to compile shader: " << dir_entry.path().filename() << "\n";
                return EXIT_FAILURE;
            }
            std::cout << "Compiled shader: " << dir_entry.path().filename() << "\n";
            gcpak_creator.addAsset(gcpak::GcpakCreator::Asset{dir_entry.path().filename().string(), binary});
        }
    }

    auto gcpak_path = shader_dir.parent_path() / "shaders.gcpak";
    if (!gcpak_creator.saveFile(gcpak_path)) {
        std::cerr << "Failed to save gcpak file shaders.gcpak!\n";
        return EXIT_FAILURE;
    }
    std::cout << "Saved shaders to " << gcpak_path << "\n";

    return EXIT_SUCCESS;
}
