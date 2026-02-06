#pragma once

#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_resources.h"

namespace gc {

class RenderTextureManager {
    struct TextureEntry {
        RenderTexture texture;
        int ref_count;
    };

    std::unordered_map<Name, TextureEntry> m_textures{};

public:
    RenderTextureManager() = default;
    RenderTextureManager(const RenderTextureManager&) = delete;
    RenderTextureManager(RenderTextureManager&&) = delete;

    RenderTextureManager& operator=(const RenderTextureManager&) = delete;
    RenderTextureManager& operator=(RenderTextureManager&&) = delete;

    RenderTexture& acquire(ResourceManager& resource_manager, RenderBackend& render_backend, Name name)
    {
        auto it = m_textures.find(name);
        if (it != m_textures.end()) {
            // Already exists, increment ref count
            it->second.ref_count += 1;
            return it->second.texture;
        }

        // Not found, create new texture
        auto inserted = m_textures.emplace(name, TextureEntry{createRenderTexture(resource_manager, render_backend, name), 1});
        return inserted.first->second.texture;
    }

    void release(Name name)
    {
        auto it = m_textures.find(name);
        GC_ASSERT(it != m_textures.end());
        it->second.ref_count -= 1;
        if (it->second.ref_count <= 0) {
            GC_ASSERT(it->second.ref_count == 0);
            m_textures.erase(it);
        }
    }

private:
    RenderTexture createRenderTexture(ResourceManager& resource_manager, RenderBackend& render_backend, Name name)
    {
        const auto& texture_resource = resource_manager.get<ResourceTexture>(name);
        return render_backend.createTexture(texture_resource.data, texture_resource.srgb);
    }
};

} // namespace gc
