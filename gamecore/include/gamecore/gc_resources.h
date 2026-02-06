#pragma once

#include <cstdlib>
#include <cstring>

#include <span>

#include <gcpak/gcpak.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_mesh_vertex.h"
#include "gamecore/gc_abort.h"

// NB
// Resources don't need to be serialisable, but they should be copyable and loadable from disk.
// TODO: To avoid resources being accidently copied, delete the copy assignment operator and add a verbose .copy() method

namespace gc {

struct ResourceTexture {
    std::span<const uint8_t> data;
    bool srgb;

    static ResourceTexture create(const Content& content_manager, Name name)
    {
        ResourceTexture tex{};
        tex.data = content_manager.findAsset(name, gcpak::GcpakAssetType::TEXTURE_R8G8B8A8);
        tex.srgb = true; // TODO read from asset
        return tex;
    }
};

struct ResourceMaterial {
    Name base_color_texture;
    Name occlusion_roughness_metallic_texture;
    Name normal_texture;

    static ResourceMaterial create(const Content& content_manager, Name name)
    {
        abortGame("Cannot load materials from disk yet"); // TODO
        return {};
    }
};

struct ResourceMesh {

    std::span<const MeshVertex> vertices;
    std::span<const uint16_t> indices;

    static ResourceMesh create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name, gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);
        GC_ASSERT(asset.size() > 2);
        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset.data(), sizeof(uint16_t));

        const uint8_t* const vertices_location = asset.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset.data() + asset.size() - indices_location) / sizeof(uint16_t);

        const std::span<const MeshVertex> vertices(reinterpret_cast<const MeshVertex*>(vertices_location), vertex_count);
        const std::span<const uint16_t> indices(reinterpret_cast<const uint16_t*>(indices_location), index_count);

        ResourceMesh mesh;
        mesh.vertices = vertices;
        mesh.indices = indices;

        return mesh;
    }
};

} // namespace gc
