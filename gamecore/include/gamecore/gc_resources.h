#pragma once

#include <cstdlib>
#include <cstring>

#include <span>
#include <optional>
#include <variant>

#include <gcpak/gcpak.h>

#include <gctemplates/gct_maybe_owning.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_mesh_vertex.h"

// NB
// Resources don't need to be serialisable, but they should be copyable and loadable from disk.

namespace gc {

struct ResourceTexture {
    gct::MaybeOwning<uint8_t> data;
    bool srgb;

    ResourceTexture() = default;

    ResourceTexture(std::vector<uint8_t> data, bool srgb) : data(std::move(data)), srgb(srgb) {}

    ResourceTexture(std::span<const uint8_t> data, bool srgb) : data(data), srgb(srgb) {}

    static std::optional<ResourceTexture> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.data.empty() || asset.type != gcpak::GcpakAssetType::TEXTURE_R8G8B8A8) {
            return {};
        }

        const bool srgb = false; // TODO read from asset
        
        return ResourceTexture(asset.data, srgb);
    }
};

struct ResourceMaterial {
    Name base_color_texture;
    Name orm_texture;
    Name normal_texture;

    static std::optional<ResourceMaterial> create(const Content& content_manager, Name name)
    {
        (void)content_manager;
        (void)name;
        return {};
    }
};

struct ResourceMesh {

    gct::MaybeOwning<MeshVertex> vertices;
    gct::MaybeOwning<uint16_t> indices;

    ResourceMesh() = default;

    ResourceMesh(std::vector<MeshVertex> vertices, std::vector<uint16_t> indices) : vertices(std::move(vertices)), indices(std::move(indices)) {}

    ResourceMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices) : vertices(vertices), indices(indices) {}

    static std::optional<ResourceMesh> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name);
        if (asset.data.empty() || asset.type != gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16) {
            return {};
        }

        GC_ASSERT(asset.data.size() > sizeof(uint16_t));

        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset.data.data(), sizeof(uint16_t));

        const uint8_t* const vertices_location = asset.data.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset.data.data() + asset.data.size() - indices_location) / sizeof(uint16_t);

        GC_ASSERT(asset.data.size() == sizeof(uint16_t) + vertex_count * sizeof(MeshVertex) + index_count * sizeof(uint16_t));

        const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
        const auto vertices_end = vertices_begin + vertex_count;
        const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
        const auto indices_end = indices_begin + index_count;

        const std::span<const MeshVertex> vertices(vertices_begin, vertices_end);
        const std::span<const uint16_t> indices(indices_begin, indices_end);

        return ResourceMesh(vertices, indices);
    }
};

} // namespace gc
