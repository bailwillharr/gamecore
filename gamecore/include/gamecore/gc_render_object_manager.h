#pragma once

#include <array>
#include <unordered_map>

#include "gamecore/gc_render_texture_manager.h"
#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_render_material.h"
#include "gamecore/gc_render_mesh.h"
#include "gamecore/gc_resources.h"

namespace gc {

// Critically, pointers returned from this object (for materials and meshes),
// must not be invalidated by later calls to .getX() (in the same frame).
// So unordered_maps to unique_ptrs are used because unordered_map can reallocate.
class RenderObjectManager {
    ResourceManager& m_resource_manager;
    RenderBackend& m_render_backend;

    RenderTextureManager m_texture_manager;

    struct MaterialEntry {
        std::unique_ptr<RenderMaterial> render_material{};
        gc::Name base_color_texture;
        gc::Name orm_texture;
        gc::Name normal_texture;
    };
    std::unordered_map<Name, MaterialEntry> m_materials{};
    std::unordered_map<Name, std::unique_ptr<RenderMesh>> m_meshes{};

    std::array<std::unique_ptr<RenderTexture>, 3> m_fallback_textures{};
    std::unique_ptr<RenderMaterial> m_fallback_material{};

public:
    RenderObjectManager(ResourceManager& resource_manager, RenderBackend& render_backend)
        : m_resource_manager(resource_manager), m_render_backend(render_backend)
    {
        std::vector<uint8_t> missing_texture(8 + 4 * 64 * 64);
        missing_texture[0] = 64; // width
        missing_texture[4] = 64; // height
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int color_index = 2 * 4 + y * 64 * 4 + x * 4;
                uint8_t& r = missing_texture[color_index + 0];
                uint8_t& g = missing_texture[color_index + 1];
                uint8_t& b = missing_texture[color_index + 2];
                r = (((x >> 3) ^ (y >> 3)) & 1) * 255;
                g = 0;
                b = r;
            }
        }
        m_fallback_textures[0] = std::make_unique<RenderTexture>(render_backend.createTexture(missing_texture, true));
        m_fallback_textures[1] =
            std::make_unique<RenderTexture>(render_backend.createTexture(std::array<uint8_t, 12>{1, 0, 0, 0, 1, 0, 0, 0, 255, 128, 0, 255}, false));
        m_fallback_textures[2] =
            std::make_unique<RenderTexture>(render_backend.createTexture(std::array<uint8_t, 12>{1, 0, 0, 0, 1, 0, 0, 0, 127, 127, 255, 255}, false));
        m_fallback_material =
            std::make_unique<RenderMaterial>(render_backend.createMaterial(*m_fallback_textures[0], *m_fallback_textures[1], *m_fallback_textures[2]));
    }
    RenderObjectManager(const RenderObjectManager&) = delete;
    RenderObjectManager(RenderObjectManager&&) = delete;

    RenderObjectManager& operator=(const RenderObjectManager&) = delete;
    RenderObjectManager& operator=(RenderObjectManager&&) = delete;

    RenderMaterial* getFallbackMaterial() const { return m_fallback_material.get(); }

    RenderMaterial* getRenderMaterial(Name name)
    {
        if (name.empty()) {
            return m_fallback_material.get();
        }
        auto it = m_materials.find(name);
        if (it == m_materials.end()) {
            // Not found, create new material
            const ResourceMaterial* material_resource = m_resource_manager.get<ResourceMaterial>(name);
            if (material_resource) {

                RenderTexture* base_color = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource->base_color_texture);
                if (!base_color) {
                    GC_ERROR("Could not find base color texture: {}", material_resource->base_color_texture);
                    base_color = m_fallback_textures[0].get();
                }
                RenderTexture* orm = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource->orm_texture);
                if (!orm) {
                    GC_ERROR("Could not find ORM texture: {}", material_resource->orm_texture);
                    orm = m_fallback_textures[1].get();
                }
                RenderTexture* normal = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource->normal_texture);
                if (!normal) {
                    GC_ERROR("Could not find normal texture: {}", material_resource->normal_texture);
                    normal = m_fallback_textures[2].get();
                }

                MaterialEntry entry{};
                entry.render_material = std::make_unique<RenderMaterial>(m_render_backend.createMaterial(*base_color, *orm, *normal));
                entry.base_color_texture = material_resource->base_color_texture;
                entry.orm_texture = material_resource->orm_texture;
                entry.normal_texture = material_resource->normal_texture;
                it = m_materials.emplace(name, std::move(entry)).first;
            }
            else {
                GC_ERROR("Could not find material resource: {}", name);
                return m_fallback_material.get();
            }
        }
        return it->second.render_material.get();
    }

    RenderMesh* getRenderMesh(Name name)
    {
        auto it = m_meshes.find(name);
        if (it != m_meshes.end()) {
            return it->second.get();
        }

        // Not found, create new mesh
        const ResourceMesh* mesh_resource = m_resource_manager.get<ResourceMesh>(name);
        if (mesh_resource) {
            auto inserted =
                m_meshes.emplace(name, std::make_unique<RenderMesh>(m_render_backend.createMesh(mesh_resource->getVertices(), mesh_resource->getIndices())));
            return inserted.first->second.get();
        }
        else {
            GC_ERROR("Could not find mesh resource: {}", name);
            return nullptr;
        }
    }

    // deletes objects not used on or after threshold_frame_index
    void deleteUnusedObjects(uint64_t threshold_frame_index)
    {
        // you can safely erase elements from an unordered_map while iterating through it.

        size_t count = 0;
        for (auto it = m_materials.begin(); it != m_materials.end();) {
            const auto& entry = it->second;
            if (entry.render_material->getLastUsedFrame() < threshold_frame_index) {
                if (!entry.base_color_texture.empty()) {
                    m_texture_manager.release(entry.base_color_texture);
                }
                if (!entry.orm_texture.empty()) {
                    m_texture_manager.release(entry.orm_texture);
                }
                if (!entry.normal_texture.empty()) {
                    m_texture_manager.release(entry.normal_texture);
                }
                it = m_materials.erase(it);
            }
            else {
                ++it;
            }
        }
        if (count > 0) {
            GC_TRACE("Deleted {} unused RenderMaterials", count);
        }

        count = std::erase_if(m_meshes, [threshold_frame_index](const auto& mesh) { return mesh.second->getLastUsedFrame() < threshold_frame_index; });
        if (count > 0) {
            GC_TRACE("Deleted {} unused RenderMeshes", count);
        }
    }
};

} // namespace gc
