#pragma once

// Makes the data of assets, in the binary formats that go in a .gcpak file (see gcpak.h). The opposite of the create() functions
// of the resources in gc_resources.h. Used by anything that makes assets: gcpak_editor, and tools that generate content.

#include <cstdint>

#include <span>
#include <vector>

#include "gamecore/gc_mesh_vertex.h"
#include "gamecore/gc_resources.h"

namespace gc {

// TEXTURE_R8G8B8A8 and TEXTURE_R8G8B8A8_SRGB. The first row of rgba is the bottom of the image
std::vector<uint8_t> makeTextureData(uint32_t width, uint32_t height, std::span<const uint8_t> rgba);

// MESH_POS12_NORM12_TANG16_UV8_INDEXED16. There can be no more than UINT16_MAX vertices
std::vector<uint8_t> makeMeshData(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices);

// CUBE_MAP_E5B9G9R9. The texels are those of every mip level, largest first, each with its six faces in the order +X -X +Y -Y +Z
// -Z, in the format VK_FORMAT_E5B9G9R9_UFLOAT_PACK32. info.face_size and info.mip_count say how many there have to be
std::vector<uint8_t> makeCubeMapData(const CubeMapInfo& info, std::span<const uint32_t> texels);

// MATERIAL. The constants of 'material' are used in place of the textures that it doesn't name
std::vector<uint8_t> makeMaterialData(const ResourceMaterial& material);

} // namespace gc
