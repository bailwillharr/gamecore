#pragma once

#include <cstdlib>
#include <cstring>

#include <span>
#include <optional>
#include <variant>

#include <gcpak/gcpak.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_mesh_vertex.h"

// NB
// Resources don't need to be serialisable, but they should be copyable and loadable from disk.

namespace gc {

struct ResourceTexture {
    std::span<const uint8_t> data;
    bool srgb;

    static std::optional<ResourceTexture> create(const Content& content_manager, Name name)
    {
        ResourceTexture tex{};
        tex.data = content_manager.findAsset(name, gcpak::GcpakAssetType::TEXTURE_R8G8B8A8);
        tex.srgb = false; // TODO read from asset

        if (tex.data.empty()) {
            return {};
        }

        return tex;
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

    struct OwningMeshData {
        std::vector<MeshVertex> vertices;
        std::vector<uint16_t> indices;
    };

    struct NonOwningMeshData {
        std::span<const MeshVertex> vertices;
        std::span<const uint16_t> indices;
    };

    std::variant<OwningMeshData, NonOwningMeshData> mesh_data;

    ResourceMesh() = default;

    ResourceMesh(std::vector<MeshVertex> vertices, std::vector<uint16_t> indices)
    {
        mesh_data.emplace<OwningMeshData>(std::move(vertices), std::move(indices));
    }

    ResourceMesh(const ResourceMesh& other, bool force_copy = true)
    {
        if (force_copy && std::holds_alternative<NonOwningMeshData>(other.mesh_data)) {
            const auto& other_mesh_data = std::get<NonOwningMeshData>(other.mesh_data);
            mesh_data.emplace<OwningMeshData>(std::vector<MeshVertex>(other_mesh_data.vertices.begin(), other_mesh_data.vertices.end()),
                                              std::vector<uint16_t>(other_mesh_data.indices.begin(), other_mesh_data.indices.end()));
        }
        else {
            mesh_data = other.mesh_data;
        }
    }

    ResourceMesh(ResourceMesh&&) = default;

    ResourceMesh& operator=(const ResourceMesh& other)
    {
        if (this != &other) {
            if (std::holds_alternative<NonOwningMeshData>(other.mesh_data)) {
                const auto& other_mesh_data = std::get<NonOwningMeshData>(other.mesh_data);
                mesh_data.emplace<OwningMeshData>(std::vector<MeshVertex>(other_mesh_data.vertices.begin(), other_mesh_data.vertices.end()),
                                                  std::vector<uint16_t>(other_mesh_data.indices.begin(), other_mesh_data.indices.end()));
            }
            else {
                mesh_data = other.mesh_data;
            }
        }
        return *this;
    }

    ResourceMesh& operator=(ResourceMesh&&) = default;

    std::span<const MeshVertex> getVertices() const
    {
        return std::visit([](auto&& data) -> std::span<const MeshVertex> { return data.vertices; }, mesh_data);
    }

    std::span<const uint16_t> getIndices() const
    {
        return std::visit([](auto&& data) -> std::span<const uint16_t> { return data.indices; }, mesh_data);
    }

    static std::optional<ResourceMesh> create(const Content& content_manager, Name name)
    {
        const auto asset = content_manager.findAsset(name, gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);
        if (asset.empty()) {
            return {};
        }

        GC_ASSERT(asset.size() > sizeof(uint16_t));

        uint16_t vertex_count{};
        std::memcpy(&vertex_count, asset.data(), sizeof(uint16_t));

        const uint8_t* const vertices_location = asset.data() + sizeof(uint16_t);
        const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
        const size_t index_count = (asset.data() + asset.size() - indices_location) / sizeof(uint16_t);

        GC_ASSERT(asset.size() == sizeof(uint16_t) + vertex_count * sizeof(MeshVertex) + index_count * sizeof(uint16_t));

        const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
        const auto vertices_end = vertices_begin + vertex_count;
        const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
        const auto indices_end = indices_begin + index_count;

        const std::span<const MeshVertex> vertices(vertices_begin, vertices_end);
        const std::span<const uint16_t> indices(indices_begin, indices_end);

        ResourceMesh mesh;
        mesh.mesh_data.emplace<NonOwningMeshData>(vertices, indices);

        return mesh;
    }
};

} // namespace gc
