#pragma once

// Turns source files (shaders, images, meshes, glTF scenes) into the assets that go in a .gcpak file.
// Nothing here needs a window or a renderer, so it is used both by the editor and by the command line (see command_line.h).
// Problems are reported through the log.

#include <cstdint>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gcpak/gcpak.h>

#include <gamecore/gc_mesh_vertex.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_resources.h>

using Asset = gcpak::GcpakCreator::Asset;

enum class SourceFileKind {
    UNKNOWN,
    SHADER,  // .vert .frag .comp   GLSL, compiled to SPIR-V
    TEXTURE, // .png .jpg .jpeg     sRGB, unless the file's name says it isn't a color. See isLinearTextureFile()
    MESH,    // .obj
    GLTF,    // .gltf .glb          becomes a prefab, along with the meshes, materials and textures it uses
};

struct CompileStats {
    uint32_t files_compiled{};
    uint32_t files_failed{};
    uint32_t assets_added{};
    uint32_t assets_replaced{};
};

// Decided by the file's extension
SourceFileKind getSourceFileKind(const std::filesystem::path& path);

// Textures are assumed to hold colors, and so to be sRGB encoded, unless the file's name (without its extension) ends with one of
// these words after a '-', '_' or '.': normal, orm, roughness, metallic, metalness, occlusion, ao, height, displacement, mask, linear.
// For example "bricks-normal.png" and "floor_orm.png" are linear and "bricks-albedo.png" is sRGB.
bool isLinearTextureFile(const std::filesystem::path& path);

inline bool isTextureType(gcpak::GcpakAssetType type)
{
    return type == gcpak::GcpakAssetType::TEXTURE_R8G8B8A8 || type == gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB;
}

// Compiles one source file and appends the resulting assets to assets_out. Most files give one asset, named after the file
// (e.g. "bricks.png"). Returns false, having added nothing, if the file couldn't be compiled.
bool compileSourceFile(const std::filesystem::path& path, std::vector<Asset>& assets_out);

// Compiles every source file in a directory and merges the results into assets. Files that aren't source files are ignored.
CompileStats compileDirectory(const std::filesystem::path& directory, bool recursive, std::vector<Asset>& assets);

// Compiles a file or a directory, whichever the path is.
CompileStats compilePath(const std::filesystem::path& path, bool recursive, std::vector<Asset>& assets);

// Moves new_assets into assets. An asset with the same ID as an existing one replaces it.
void mergeAssets(std::vector<Asset>& assets, std::vector<Asset>&& new_assets, CompileStats& stats);

// The ID that the asset has in a .gcpak file
uint32_t getAssetId(const Asset& asset);

Asset makeAsset(const std::string& name, std::vector<uint8_t> data, gcpak::GcpakAssetType type);

// The binary formats of assets. See gcpak.h
std::vector<uint8_t> makeTextureData(uint32_t width, uint32_t height, std::span<const uint8_t> rgba);
std::vector<uint8_t> makeMeshData(std::span<const gc::MeshVertex> vertices, std::span<const uint16_t> indices);
// The constants of 'material' are used in place of the textures that it doesn't name
std::vector<uint8_t> makeMaterialData(const gc::ResourceMaterial& material);
