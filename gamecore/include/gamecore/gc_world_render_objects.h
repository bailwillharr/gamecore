#pragma once

#include <unordered_map>

#include "gamecore/gc_render_texture_manager.h"
#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_render_material.h"
#include "gamecore/gc_render_mesh.h"
#include "gamecore/gc_resources.h"

namespace gc {

// Critically, pointers returned from this object (for materials and meshes),
// must not be invalidated by later calls to .getX().
// So unordered_maps to unique_ptrs are used because unordered_map can reallocate.
class WorldRenderObjects {
    ResourceManager& m_resource_manager;
    RenderBackend& m_render_backend;

    RenderTextureManager m_texture_manager;
    std::unordered_map<Name, std::unique_ptr<RenderMaterial>> m_materials{};
    std::unordered_map<Name, std::unique_ptr<RenderMesh>> m_meshes{};

public:
    WorldRenderObjects(ResourceManager& resource_manager, RenderBackend& render_backend)
        : m_resource_manager(resource_manager), m_render_backend(render_backend)
    {
    }
    WorldRenderObjects(const WorldRenderObjects&) = delete;
    WorldRenderObjects(WorldRenderObjects&&) = delete;

    WorldRenderObjects& operator=(const WorldRenderObjects&) = delete;
    WorldRenderObjects& operator=(WorldRenderObjects&&) = delete;

    RenderMaterial* getRenderMaterial(Name name)
    {
        auto it = m_materials.find(name);
        if (it != m_materials.end()) {
            return it->second.get();
        }

        // Not found, create new material
        const auto& material_resource = m_resource_manager.get<ResourceMaterial>(name);

        auto& base_color = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource.base_color_texture);
        auto& orm = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource.occlusion_roughness_metallic_texture);
        auto& normal = m_texture_manager.acquire(m_resource_manager, m_render_backend, material_resource.normal_texture);

        auto inserted = m_materials.emplace(name, std::make_unique<RenderMaterial>(m_render_backend.createMaterial(base_color, orm, normal)));
        return inserted.first->second.get();
    }

    RenderMesh* getRenderMesh(Name name)
    {
        auto it = m_meshes.find(name);
        if (it != m_meshes.end()) {
            return it->second.get();
        }

        // Not found, create new mesh
        const auto& mesh_resource = m_resource_manager.get<ResourceMesh>(name);

        auto inserted = m_meshes.emplace(name, std::make_unique<RenderMesh>(m_render_backend.createMesh(mesh_resource.vertices, mesh_resource.indices)));
        return inserted.first->second.get();
    }
};

} // namespace gc
