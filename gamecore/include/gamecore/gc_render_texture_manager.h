#pragma once

#include "gamecore/gc_resource_manager.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_resources.h"

namespace gc {

// How the data of a resource should be uploaded when a texture or a mesh is made from it (see RenderUploadMode).
// A resource that was loaded from the content is a view of a mapped .gcpak file, which stays put for as long as the program runs,
// so it can be read by the render work thread whenever that gets to it. A resource with data of its own (one that the game
// generated, say) can be deleted or changed at any time, so its data is copied straight away.
template <typename T>
RenderUploadMode getRenderUploadMode(const ResourceManager& resource_manager, std::span<const T> resource_data)
{
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(resource_data.data()), resource_data.size_bytes());
    return resource_manager.getContent().contains(bytes) ? RenderUploadMode::BACKGROUND : RenderUploadMode::IMMEDIATE;
}

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

    // Do not call release() if this fails.
    // Returns nullptr on failure.
    RenderTexture* acquire(ResourceManager& resource_manager, RenderBackend& render_backend, Name name)
    {
        auto it = m_textures.find(name);
        if (it != m_textures.end()) {
            // Already exists, increment ref count
            it->second.ref_count += 1;
            return &it->second.texture;
        }

        // Not found, create new texture
        const ResourceTexture* texture_resource = resource_manager.get<ResourceTexture>(name);
        if (!texture_resource) {
            return nullptr;
        }
        auto inserted = m_textures.emplace(name, TextureEntry{createRenderTexture(resource_manager, render_backend, *texture_resource), 1});
        return &inserted.first->second.texture;
    }

    void release(Name name)
    {
        auto it = m_textures.find(name);
        if (it != m_textures.end()) {
            it->second.ref_count -= 1;
            if (it->second.ref_count <= 0) {
                GC_ASSERT(it->second.ref_count == 0);
                m_textures.erase(it);
            }
        }
    }

private:
    RenderTexture createRenderTexture(const ResourceManager& resource_manager, RenderBackend& render_backend, const ResourceTexture& texture)
    {
        return render_backend.createTexture(texture.data.get(), texture.srgb, getRenderUploadMode(resource_manager, texture.data.get()));
    }
};

} // namespace gc
