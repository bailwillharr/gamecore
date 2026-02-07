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

        if (tex.data.empty()) {
            gc::abortGame("Could not find asset");
        }

        return tex;
    }
};

struct ResourceMaterial {
    Name base_color_texture;
    Name occlusion_roughness_metallic_texture;
    Name normal_texture;

    static ResourceMaterial create(const Content& content_manager, Name name)
    {
        (void)content_manager;
        (void)name;
        abortGame("Cannot load materials from disk yet"); // TODO
        // return {};
    }
};

struct ResourceMesh {

    std::vector<MeshVertex> vertices;
    std::vector<uint16_t> indices;

    static ResourceMesh create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name, gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);
        if (asset.empty()) {
            gc::abortGame("Could not find asset");
        }

        GC_ASSERT(asset.size() > 2);

        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset.data(), sizeof(uint16_t));

        const uint8_t* const vertices_location = asset.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset.data() + asset.size() - indices_location) / sizeof(uint16_t);

        const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
        const auto vertices_end = vertices_begin + vertex_count;
        const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
        const auto indices_end = indices_begin + index_count;

        ResourceMesh mesh;
        mesh.vertices.insert(mesh.vertices.begin(), vertices_begin, vertices_end);
        mesh.indices.insert(mesh.indices.begin(), indices_begin, indices_end);
        return mesh;
    }
};

} // namespace gc
