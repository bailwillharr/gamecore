#pragma once

// RmlUi draws through this (see gc_ui.h). It doesn't draw anything itself: it writes what RmlUi asks for into a UIDrawData, which
// the RenderBackend draws later in the frame. Private to the engine.

#include <cstdint>

#include <memory>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <RmlUi/Core/RenderInterface.h>

#include "gamecore/gc_ui_draw_data.h"

namespace gc {

class RenderBackend;   // forward-dec
class RenderUITexture; // forward-dec
class UIFileInterface; // forward-dec

class UIRenderInterface final : public Rml::RenderInterface {
    // RmlUi's geometry stays on the CPU. It is copied into the frame's UIDrawData when it is drawn (UIs have little of it)
    struct Geometry {
        std::vector<UIVertex> vertices;
        std::vector<uint32_t> indices;
        // where it is in the frame's UIDrawData, if it has been drawn in this frame
        uint64_t frame;
        uint32_t first_index;
        int32_t vertex_offset;
    };

    struct Shader {
        UIGradient gradient;
        std::unique_ptr<RenderUITexture> ramp; // the gradient's colors
    };

    RenderBackend& m_render_backend;
    UIFileInterface& m_files;

    UIDrawData* m_draw_data{}; // only while RmlUi is drawing
    uint64_t m_frame{};
    glm::ivec2 m_size{};

    bool m_scissor_enabled{};
    glm::ivec4 m_scissor{};
    bool m_clip_mask_enabled{};
    uint8_t m_clip_mask_value{1};
    int32_t m_transform{-1};

    // Textures that RmlUi has finished with, which may still be in a UIDrawData that hasn't been drawn yet
    std::vector<std::unique_ptr<RenderUITexture>> m_released_textures{};

public:
    UIRenderInterface(RenderBackend& render_backend, UIFileInterface& files);
    UIRenderInterface(const UIRenderInterface&) = delete;
    UIRenderInterface(UIRenderInterface&&) = delete;

    ~UIRenderInterface() override;

    UIRenderInterface& operator=(const UIRenderInterface&) = delete;
    UIRenderInterface& operator=(UIRenderInterface&&) = delete;

    // Around Rml::Context::Render(). size is the context's
    void beginFrame(UIDrawData& draw_data, glm::ivec2 size);
    void endFrame();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

private:
    void addDraw(Geometry& geometry, UIDrawKind kind, RenderUITexture* texture, Rml::Vector2f translation, const UIGradient* gradient);
};

} // namespace gc
