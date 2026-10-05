#define _CRT_SECURE_NO_WARNINGS

#include "asset_compiler.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

#include <glm/geometric.hpp>

#include <shaderc/shaderc.hpp>

#include <stb_image.h>

#include <gclog/gclog.h>

#include <gamecore/gc_crc_table.h>
#include <gamecore/gc_gen_tangents.h>

#include "gltf_compiler.h"

static std::string getLowercaseExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.cbegin(), ext.cend(), ext.begin(), [](char c) -> char { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return ext;
}

SourceFileKind getSourceFileKind(const std::filesystem::path& path)
{
    const std::string ext = getLowercaseExtension(path);
    if (ext == ".vert" || ext == ".frag" || ext == ".comp") {
        return SourceFileKind::SHADER;
    }
    else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
        return SourceFileKind::TEXTURE;
    }
    else if (ext == ".obj") {
        return SourceFileKind::MESH;
    }
    else if (ext == ".gltf" || ext == ".glb") {
        return SourceFileKind::GLTF;
    }
    else {
        return SourceFileKind::UNKNOWN;
    }
}

bool isLinearTextureFile(const std::filesystem::path& path)
{
    std::string stem = path.stem().string();
    std::transform(stem.cbegin(), stem.cend(), stem.begin(), [](char c) -> char { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    for (const std::string_view word :
         {"normal", "orm", "roughness", "metallic", "metalness", "occlusion", "ao", "height", "displacement", "mask", "linear"}) {
        if (stem.size() > word.size() && stem.ends_with(word)) {
            const char separator = stem[stem.size() - word.size() - 1];
            if (separator == '-' || separator == '_' || separator == '.') {
                return true;
            }
        }
    }
    return false;
}

uint32_t getAssetId(const Asset& asset) { return asset.name.empty() ? asset.hash : gc::crc32_impl(asset.name.c_str()); }

Asset makeAsset(const std::string& name, std::vector<uint8_t> data, gcpak::GcpakAssetType type)
{
    Asset asset{};
    asset.name = name;
    asset.hash = gc::crc32_impl(name.c_str());
    asset.data = std::move(data);
    asset.type = type;
    return asset;
}

std::vector<uint8_t> makeTextureData(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
{
    std::vector<uint8_t> data(2 * sizeof(uint32_t) + rgba.size());
    std::memcpy(data.data(), &width, sizeof(uint32_t));
    std::memcpy(data.data() + sizeof(uint32_t), &height, sizeof(uint32_t));
    std::memcpy(data.data() + 2 * sizeof(uint32_t), rgba.data(), rgba.size());
    return data;
}

std::vector<uint8_t> makeMeshData(std::span<const gc::MeshVertex> vertices, std::span<const uint16_t> indices)
{
    const uint16_t num_vertices = static_cast<uint16_t>(vertices.size());
    std::vector<uint8_t> data(sizeof(uint16_t) + vertices.size_bytes() + indices.size_bytes());
    std::memcpy(data.data(), &num_vertices, sizeof(uint16_t));
    std::memcpy(data.data() + sizeof(uint16_t), vertices.data(), vertices.size_bytes());
    std::memcpy(data.data() + sizeof(uint16_t) + vertices.size_bytes(), indices.data(), indices.size_bytes());
    return data;
}

std::vector<uint8_t> makeMaterialData(const gc::ResourceMaterial& material)
{
    const uint32_t ids[3]{material.base_color_texture.getHash(), material.orm_texture.getHash(), material.normal_texture.getHash()};
    const float constants[6]{material.base_color.r, material.base_color.g, material.base_color.b,
                             material.base_color.a, material.roughness,    material.metallic};
    const uint32_t blend_mode = static_cast<uint32_t>(material.blend_mode);
    static_assert(sizeof(ids) == gc::ResourceMaterial::TEXTURES_SIZE && sizeof(constants) == gc::ResourceMaterial::CONSTANTS_SIZE);
    static_assert(sizeof(blend_mode) + sizeof(material.alpha_cutoff) == gc::ResourceMaterial::BLEND_SIZE);
    const uint32_t emissive_id = material.emissive_texture.getHash();
    const float emissive[3]{material.emissive.r, material.emissive.g, material.emissive.b};
    static_assert(sizeof(emissive_id) + sizeof(emissive) == gc::ResourceMaterial::EMISSIVE_SIZE);
    std::vector<uint8_t> data(sizeof(ids) + sizeof(constants) + gc::ResourceMaterial::BLEND_SIZE + gc::ResourceMaterial::EMISSIVE_SIZE);
    uint8_t* dest = data.data();
    std::memcpy(dest, ids, sizeof(ids));
    dest += sizeof(ids);
    std::memcpy(dest, constants, sizeof(constants));
    dest += sizeof(constants);
    std::memcpy(dest, &blend_mode, sizeof(blend_mode));
    dest += sizeof(blend_mode);
    std::memcpy(dest, &material.alpha_cutoff, sizeof(material.alpha_cutoff));
    dest += sizeof(material.alpha_cutoff);
    std::memcpy(dest, &emissive_id, sizeof(emissive_id));
    dest += sizeof(emissive_id);
    std::memcpy(dest, emissive, sizeof(emissive));
    return data;
}

//
// Shaders
//

static std::vector<uint8_t> compileShader(const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();
    const std::string ext = getLowercaseExtension(path);

    shaderc_shader_kind kind{};
    if (ext == ".vert") {
        kind = shaderc_vertex_shader;
    }
    else if (ext == ".frag") {
        kind = shaderc_fragment_shader;
    }
    else {
        kind = shaderc_compute_shader;
    }

    std::ifstream source_file{path};
    if (!source_file) {
        GC_ERROR("Failed to open shader source: {}", filename);
        return {};
    }
    std::ostringstream source{};
    source << source_file.rdbuf();

    // creating the compiler is slow, so it is kept for the next shader
    static const shaderc::Compiler s_compiler{};
    if (!s_compiler.IsValid()) {
        GC_ERROR("Failed to initialise the shader compiler");
        return {};
    }

    shaderc::CompileOptions options{};
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetAutoBindUniforms(false);
    options.SetWarningsAsErrors();

    const shaderc::SpvCompilationResult result = s_compiler.CompileGlslToSpv(source.str(), kind, filename.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        GC_ERROR("Failed to compile shader {}:\n{}", filename, result.GetErrorMessage());
        return {};
    }

    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(result.cbegin()), reinterpret_cast<const uint8_t*>(result.cend()));
}

//
// Textures
//

static std::vector<uint8_t> compileTexture(const std::filesystem::path& path)
{
    int width{}, height{}, channels_in_file{};
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(path.string().c_str(), &width, &height, &channels_in_file, 4),
                                                                       stbi_image_free);
    if (!pixels || width <= 0 || height <= 0) {
        GC_ERROR("Failed to read image {}: {}", path.filename().string(), stbi_failure_reason());
        return {};
    }

    // The engine expects data to start at the bottom-left.
    // This is because Vulkan samplers treat uv=0,0 as the start of the image data, and OBJ models assume uv 0,0 is at the bottom-left.
    // (The rows are flipped here rather than with stbi_set_flip_vertically_on_load(), which would affect every user of stb_image.)
    const size_t row_size = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> flipped(row_size * static_cast<size_t>(height));
    for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
        std::memcpy(flipped.data() + y * row_size, pixels.get() + (static_cast<size_t>(height) - 1 - y) * row_size, row_size);
    }

    return makeTextureData(static_cast<uint32_t>(width), static_cast<uint32_t>(height), flipped);
}

//
// OBJ meshes
//

static std::vector<uint8_t> compileObjMesh(const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();

    std::ifstream file(path);
    if (!file) {
        GC_ERROR("Failed to open mesh: {}", filename);
        return {};
    }

    std::vector<glm::vec3> positions{};
    std::vector<glm::vec2> uvs{};
    std::vector<glm::vec3> normals{};
    std::vector<gc::MeshVertex> vertices{}; // three per triangle

    std::string line{};
    for (int line_number = 1; std::getline(file, line); ++line_number) {
        const auto fail = [&](const char* what) {
            GC_ERROR("{}:{}: {}", filename, line_number, what);
            return std::vector<uint8_t>{};
        };

        if (line.starts_with("v ")) {
            float x{}, y{}, z{};
            if (std::sscanf(line.c_str(), "v %f %f %f", &x, &y, &z) != 3) {
                return fail("invalid position");
            }
            positions.emplace_back(x, -z, y); // use Z-up instead
        }
        else if (line.starts_with("vt ")) {
            float u{}, v{};
            if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) != 2) {
                return fail("invalid texture coordinate");
            }
            uvs.emplace_back(u, v);
        }
        else if (line.starts_with("vn ")) {
            float x{}, y{}, z{};
            if (std::sscanf(line.c_str(), "vn %f %f %f", &x, &y, &z) != 3) {
                return fail("invalid normal");
            }
            normals.push_back(glm::normalize(glm::vec3{x, -z, y})); // use Z-up instead
        }
        else if (line.starts_with("f ")) {
            int indices[3][3]{};
            if (std::sscanf(line.c_str(), "f %d/%d/%d %d/%d/%d %d/%d/%d", &indices[0][0], &indices[0][1], &indices[0][2], &indices[1][0], &indices[1][1],
                            &indices[1][2], &indices[2][0], &indices[2][1], &indices[2][2]) != 9) {
                return fail("invalid face. Faces must be triangles with positions, texture coordinates and normals");
            }
            for (int i = 0; i < 3; ++i) {
                // indices start at one. Negative (relative) indices aren't supported
                const size_t position_index = static_cast<size_t>(indices[i][0]) - 1;
                const size_t uv_index = static_cast<size_t>(indices[i][1]) - 1;
                const size_t normal_index = static_cast<size_t>(indices[i][2]) - 1;
                if (indices[i][0] <= 0 || indices[i][1] <= 0 || indices[i][2] <= 0 || position_index >= positions.size() || uv_index >= uvs.size() ||
                    normal_index >= normals.size()) {
                    return fail("invalid index in face");
                }
                gc::MeshVertex vertex{};
                vertex.position = positions[position_index];
                vertex.uv = uvs[uv_index];
                vertex.normal = normals[normal_index];
                vertices.push_back(vertex);
            }
        }
    }

    if (vertices.empty()) {
        GC_ERROR("No triangles found in mesh: {}", filename);
        return {};
    }

    // also removes duplicate vertices
    const std::vector<int> remap = gc::genTangents(vertices);
    if (vertices.size() > UINT16_MAX) {
        GC_ERROR("Mesh {} has {} vertices. Meshes can have no more than {}", filename, vertices.size(), UINT16_MAX);
        return {};
    }
    std::vector<uint16_t> indices{};
    indices.reserve(remap.size());
    for (const int index : remap) {
        indices.push_back(static_cast<uint16_t>(index));
    }

    return makeMeshData(vertices, indices);
}

//
//
//

bool compileSourceFile(const std::filesystem::path& path, std::vector<Asset>& assets_out)
{
    const std::string name = path.filename().string();
    std::vector<uint8_t> data{};
    gcpak::GcpakAssetType type{};

    switch (getSourceFileKind(path)) {
    case SourceFileKind::SHADER:
        data = compileShader(path);
        type = gcpak::GcpakAssetType::SPIRV_SHADER;
        break;
    case SourceFileKind::TEXTURE:
        data = compileTexture(path);
        type = isLinearTextureFile(path) ? gcpak::GcpakAssetType::TEXTURE_R8G8B8A8 : gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB;
        break;
    case SourceFileKind::MESH:
        data = compileObjMesh(path);
        type = gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16;
        break;
    case SourceFileKind::GLTF:
        return compileGltf(path, assets_out);
    case SourceFileKind::UNKNOWN:
        GC_ERROR("Don't know how to compile {}", name);
        return false;
    }

    if (data.empty()) {
        return false; // the reason has been logged
    }
    assets_out.push_back(makeAsset(name, std::move(data), type));
    return true;
}

void mergeAssets(std::vector<Asset>& assets, std::vector<Asset>&& new_assets, CompileStats& stats)
{
    for (Asset& new_asset : new_assets) {
        const uint32_t id = getAssetId(new_asset);
        const auto existing = std::find_if(assets.begin(), assets.end(), [id](const Asset& asset) { return getAssetId(asset) == id; });
        if (existing != assets.end()) {
            if (existing->name != new_asset.name && !existing->name.empty()) {
                GC_WARN("{} and {} have the same asset ID. Keeping {}", existing->name, new_asset.name, new_asset.name);
            }
            *existing = std::move(new_asset);
            ++stats.assets_replaced;
        }
        else {
            assets.push_back(std::move(new_asset));
            ++stats.assets_added;
        }
    }
    new_assets.clear();
}

static void compileAndMerge(const std::filesystem::path& file, std::vector<Asset>& assets, CompileStats& stats)
{
    std::vector<Asset> new_assets{};
    if (compileSourceFile(file, new_assets)) {
        GC_INFO("Compiled {} ({} asset{})", file.filename().string(), new_assets.size(), new_assets.size() == 1 ? "" : "s");
        ++stats.files_compiled;
        mergeAssets(assets, std::move(new_assets), stats);
    }
    else {
        ++stats.files_failed;
    }
}

CompileStats compileDirectory(const std::filesystem::path& directory, bool recursive, std::vector<Asset>& assets)
{
    CompileStats stats{};

    // Sorted, so that the result doesn't depend on the order the filesystem lists files in.
    std::vector<std::filesystem::path> files{};
    std::error_code ec{};
    const auto consider = [&](const std::filesystem::directory_entry& entry) {
        if (entry.is_regular_file(ec) && getSourceFileKind(entry.path()) != SourceFileKind::UNKNOWN) {
            files.push_back(entry.path());
        }
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end{};
             !ec && it != end; it.increment(ec)) {
            consider(*it);
        }
    }
    else {
        for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end{}; !ec && it != end;
             it.increment(ec)) {
            consider(*it);
        }
    }
    if (ec) {
        GC_ERROR("Failed to read directory {}: {}", directory.string(), ec.message());
        ++stats.files_failed;
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        compileAndMerge(file, assets, stats);
    }
    return stats;
}

CompileStats compilePath(const std::filesystem::path& path, bool recursive, std::vector<Asset>& assets)
{
    std::error_code ec{};
    if (std::filesystem::is_directory(path, ec)) {
        return compileDirectory(path, recursive, assets);
    }
    CompileStats stats{};
    if (!std::filesystem::is_regular_file(path, ec)) {
        GC_ERROR("No such file or directory: {}", path.string());
        ++stats.files_failed;
        return stats;
    }
    compileAndMerge(path, assets, stats);
    return stats;
}
