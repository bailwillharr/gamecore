//
// compile_shaders.exe
//

#include "compile_shaders.h"

#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>

#include <shaderc/shaderc.hpp>

#include <gcpak/gcpak.h>

static std::optional<shaderc_shader_kind> determineShaderKind(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.cbegin(), ext.cend(), ext.begin(), [](char c) -> char { return static_cast<char>(tolower(c)); });
    if (ext == std::string(".vert")) {
        return shaderc_vertex_shader;
    }
    else if (ext == std::string(".frag")) {
        return shaderc_fragment_shader;
    }
    else if (ext == std::string(".comp")) {
        return shaderc_compute_shader;
    }
    else {
        return {};
    }
}

static std::vector<uint8_t> compileShader(const shaderc::Compiler& compiler, const std::filesystem::path& path)
{
    const auto filename = path.filename().string();

    shaderc_shader_kind kind{};
    if (auto kind_opt = determineShaderKind(path); kind_opt) {
        kind = kind_opt.value();
    }
    else {
        std::cerr << "Shader source has invalid extension: " << filename << "\n";
        return {};
    }

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

    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(compiledShader.cbegin()), reinterpret_cast<const uint8_t*>(compiledShader.cend()));
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    std::error_code ec{};

    const auto shader_dir = std::filesystem::path(COMPILE_SHADERS_SOURCE_DIRECTORY).parent_path().parent_path() / "content" / "shader_src";
    if (!std::filesystem::exists(shader_dir, ec) || !std::filesystem::is_directory(shader_dir, ec)) {
        std::cerr << "Failed to find shader_src directory! error: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    const auto gcpak_path = shader_dir.parent_path() / "shaders.gcpak";
    // const auto gcpak_date_modified = std::filesystem::exists(gcpak_path) ? std::filesystem::last_write_time(gcpak_path) :
    // std::filesystem::file_time_type::min();

    // find all regular files in the directory and compile them
    const shaderc::Compiler compiler{};
    if (!compiler.IsValid()) {
        std::cerr << "Failed to initialise shaderc compiler!\n";
        return EXIT_FAILURE;
    }
    gcpak::GcpakCreator gcpak_creator{};
    for (const auto& dir_entry : std::filesystem::directory_iterator(shader_dir)) {

        if (!dir_entry.is_regular_file()) {
            continue;
        }

        if (!determineShaderKind(dir_entry.path())) {
            continue;
        }

        // (std::filesystem::last_write_time(dir_entry.path()) <= gcpak_date_modified) {
        //    std::cout << "Shader up to date: " << dir_entry.path().filename() << "\n";
        //    continue;
        //}

        auto binary = compileShader(compiler, dir_entry);
        if (binary.empty()) {
            std::cerr << "Failed to compile shader: " << dir_entry.path().filename() << "\n";
            continue;
        }

        std::cout << "Compiled shader: " << dir_entry.path().filename() << "\n";
        gcpak_creator.addAsset(gcpak::GcpakCreator::Asset{dir_entry.path().filename().string(), binary, gcpak::GcpakAssetType::SPIRV_SHADER});
    }

    if (!gcpak_creator.saveFile(gcpak_path)) {
        std::cerr << "Failed to save gcpak file shaders.gcpak!\n";
        return EXIT_FAILURE;
    }

    std::cout << "Saved shaders to " << gcpak_path << "\n";

    { // wait for enter before exit
        std::cout << "Press enter to exit\n";
        int c{};
        do {
            c = getchar();
        } while (c != '\n' && c != EOF);
    }

    return EXIT_SUCCESS;
}
