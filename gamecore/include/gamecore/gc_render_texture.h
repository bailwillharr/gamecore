#pragma once

#include "gamecore/gc_gpu_resources.h"

#include "gamecore/gc_logger.h"

namespace gc {

class RenderTexture {

    GPUTexture m_texture;
    mutable bool m_uploaded;

public:
    explicit RenderTexture(GPUTexture&& texture) : m_texture(std::move(texture)), m_uploaded(false) {}
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture(RenderTexture&& other) noexcept : m_texture(std::move(other.m_texture)), m_uploaded(other.m_uploaded) {}

    RenderTexture& operator=(const RenderTexture&) = delete;
    RenderTexture& operator=(RenderTexture&&) = delete;

    bool isUploaded() const
    {
        if (m_uploaded) {
            return true;
        }
        // if the backing image is no longer in use by the queue, assuming the backing image was just created, this means the image is uploaded.
        if (m_texture.isFree()) {
            GC_DEBUG("RenderTexture uploaded: {}", reinterpret_cast<void*>(m_texture.getImage()));
            m_uploaded = true;
            return true;
        }
        return false;
    }

    void waitForUpload() const
    {
        if (!m_uploaded) {
            m_texture.waitForFree();
            m_uploaded = true;
        }
    }

    VkImageView getImageView() const { return m_texture.getImageView(); }

    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        m_texture.useResource(timeline_semaphore, resource_free_signal_value);
    }
};

} // namespace gc
