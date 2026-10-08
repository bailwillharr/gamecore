#pragma once

#include <optional>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_uploader.h"

#include "gclog/gclog.h"

namespace gc {

// A texture on the GPU. It can't be used for anything until isUploaded(): until then its texels are on their way to the GPU,
// and it may not even have an image yet (see RenderUploader).
class RenderTexture {

    // (mutable because finding out that the upload has moved on changes them)
    mutable std::optional<GPUTexture> m_texture{}; // empty until the upload has made the image and given it to the GPU to fill
    mutable RenderUpload m_upload;
    mutable bool m_uploaded = false;

public:
    explicit RenderTexture(RenderUpload&& upload) : m_upload(std::move(upload))
    {
        takeTexture();
        GC_TRACE("Created RenderTexture");
    }
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture(RenderTexture&& other) noexcept
        : m_texture(std::move(other.m_texture)), m_upload(std::move(other.m_upload)), m_uploaded(other.m_uploaded)
    {
    }

    ~RenderTexture()
    {
        // if the GPU is copying into the image, the image is taken so that it isn't destroyed until that has finished
        takeTexture();
        GC_TRACE("Destroyed RenderTexture");
    }

    RenderTexture& operator=(const RenderTexture&) = delete;
    RenderTexture& operator=(RenderTexture&&) = delete;

    bool isUploaded() const
    {
        if (m_uploaded) {
            return true;
        }
        if (!takeTexture()) {
            return false; // the texels are still being copied to a staging buffer, and the GPU hasn't been given them yet
        }
        // if the backing image is no longer in use by the queue, assuming the backing image was just created, this means the image is uploaded.
        if (m_texture->isFree()) {
            GC_TRACE("RenderTexture uploaded: {}", reinterpret_cast<void*>(m_texture->getImage()));
            m_uploaded = true;
            return true;
        }
        return false;
    }

    void waitForUpload() const
    {
        if (!m_uploaded) {
            m_upload.waitForSubmit();
            takeTexture();
            m_texture->waitForFree();
            m_uploaded = true;
        }
    }

    // Only once isUploaded()
    VkImageView getImageView() const
    {
        GC_ASSERT(m_texture.has_value());
        return m_texture->getImageView();
    }

    // Only once isUploaded()
    void useResource(VkSemaphore timeline_semaphore, uint64_t resource_free_signal_value)
    {
        GC_ASSERT(m_texture.has_value());
        m_texture->useResource(timeline_semaphore, resource_free_signal_value);
    }

private:
    // Gets the image from the upload, if the upload has got as far as giving it to the GPU. Returns whether there is an image
    bool takeTexture() const
    {
        if (m_texture) {
            return true;
        }
        if (m_upload.isDone() || !m_upload.isSubmitted()) {
            return false; // (done without an image: this object was moved from)
        }
        m_texture.emplace(m_upload.takeTexture());
        return true;
    }
};

} // namespace gc
