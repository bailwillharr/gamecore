#pragma once

#include "gamecore/gc_gpu_resources.h"

namespace gc {

class RenderTexture {

    GPUImageView m_image_view;
    mutable bool m_uploaded;

public:
    explicit RenderTexture(GPUImageView&& image_view) : m_image_view(std::move(image_view)), m_uploaded(false) {}
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture(RenderTexture&& other) noexcept : m_image_view(std::move(other.m_image_view)), m_uploaded(other.m_uploaded) {}

    RenderTexture& operator=(const RenderTexture&) = delete;
    RenderTexture& operator=(RenderTexture&&) = delete;

    bool isUploaded() const
    {
        if (m_uploaded) {
            return true;
        }
        // if the backing image is no longer in use by the queue, assuming the backing image was just created, this means the image is uploaded.
        if (m_image_view.getImage()->isFree()) {
            m_uploaded = true;
            return true;
        }
        return false;
    }

    GPUImageView& getImageView() { return m_image_view; }
};

} // namespace gc