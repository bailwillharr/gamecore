#include "gc_ui_render_interface.h"

#include <cmath>
#include <cstring>

#include <algorithm>
#include <array>
#include <limits>

#include <glm/mat4x4.hpp>

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Variant.h>
#include <RmlUi/Core/Vertex.h>

#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_render_ui.h"
#include "gc_ui_interfaces.h"

namespace gc {

static_assert(sizeof(Rml::Vertex) == sizeof(UIVertex));
static_assert(offsetof(Rml::Vertex, position) == offsetof(UIVertex, position));
static_assert(offsetof(Rml::Vertex, colour) == offsetof(UIVertex, color));
static_assert(offsetof(Rml::Vertex, tex_coord) == offsetof(UIVertex, uv));
static_assert(sizeof(Rml::Matrix4f) == sizeof(glm::mat4));

// The number of texels in a gradient's color ramp
static constexpr uint32_t GRADIENT_RAMP_SIZE = 256;

UIRenderInterface::UIRenderInterface(RenderBackend& render_backend, UIFileInterface& files) : m_render_backend(render_backend), m_files(files) {}

UIRenderInterface::~UIRenderInterface() = default;

void UIRenderInterface::beginFrame(UIDrawData& draw_data, glm::ivec2 size)
{
    // Last frame's UIDrawData has been drawn by now (the RenderBackend keeps the textures alive until the GPU has finished with them)
    m_released_textures.clear();

    ++m_frame;
    m_draw_data = &draw_data;
    m_draw_data->setSize(size);
    m_size = size;
    m_scissor_enabled = false;
    m_clip_mask_enabled = false;
    m_clip_mask_value = 1;
    m_transform = -1;
}

void UIRenderInterface::endFrame() { m_draw_data = nullptr; }

Rml::CompiledGeometryHandle UIRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    auto* const geometry = new Geometry{};
    geometry->vertices.resize(vertices.size());
    if (!vertices.empty()) {
        std::memcpy(geometry->vertices.data(), vertices.data(), vertices.size() * sizeof(UIVertex));
    }
    geometry->indices.assign(indices.begin(), indices.end());
    geometry->frame = std::numeric_limits<uint64_t>::max();
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void UIRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    addDraw(*reinterpret_cast<Geometry*>(geometry), UIDrawKind::TRIANGLES, reinterpret_cast<RenderUITexture*>(texture), translation, nullptr);
}

void UIRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { delete reinterpret_cast<Geometry*>(geometry); }

// RmlUi expects textures to have their alpha premultiplied
static void premultiplyAlpha(std::vector<uint8_t>& texels)
{
    for (size_t i = 0; i + 3 < texels.size(); i += 4) {
        const uint32_t alpha = texels[i + 3];
        for (size_t c = 0; c < 3; ++c) {
            texels[i + c] = static_cast<uint8_t>((texels[i + c] * alpha + 127) / 255);
        }
    }
}

Rml::TextureHandle UIRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    std::vector<uint8_t> texels{};
    uint32_t width{}, height{};
    if (!m_files.loadImage(source, texels, width, height)) {
        return {};
    }
    premultiplyAlpha(texels);
    texture_dimensions = Rml::Vector2i(static_cast<int>(width), static_cast<int>(height));
    // (mipmapped, as images are often drawn smaller than they are)
    return reinterpret_cast<Rml::TextureHandle>(m_render_backend.createUITexture(texels, width, height, true).release());
}

Rml::TextureHandle UIRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0) {
        return {};
    }
    const std::span<const uint8_t> texels(reinterpret_cast<const uint8_t*>(source.data()), source.size());
    return reinterpret_cast<Rml::TextureHandle>(
        m_render_backend.createUITexture(texels, static_cast<uint32_t>(source_dimensions.x), static_cast<uint32_t>(source_dimensions.y), false)
            .release());
}

void UIRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    if (texture) {
        m_released_textures.emplace_back(reinterpret_cast<RenderUITexture*>(texture));
    }
}

void UIRenderInterface::EnableScissorRegion(bool enable) { m_scissor_enabled = enable; }

void UIRenderInterface::SetScissorRegion(Rml::Rectanglei region) { m_scissor = glm::ivec4(region.Left(), region.Top(), region.Width(), region.Height()); }

void UIRenderInterface::EnableClipMask(bool enable) { m_clip_mask_enabled = enable; }

void UIRenderInterface::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    // As RmlUi's OpenGL 3 renderer: what is drawn afterwards passes where the stencil buffer is m_clip_mask_value
    UIDrawKind kind{};
    switch (operation) {
    case Rml::ClipMaskOperation::Set:
        kind = UIDrawKind::CLIP_SET; // clears to 0, writes 1
        m_clip_mask_value = 1;
        break;
    case Rml::ClipMaskOperation::SetInverse:
        kind = UIDrawKind::CLIP_SET_INVERSE; // clears to 1, writes 0
        m_clip_mask_value = 1;
        break;
    case Rml::ClipMaskOperation::Intersect:
        kind = UIDrawKind::CLIP_INTERSECT; // adds 1
        m_clip_mask_value = static_cast<uint8_t>(std::min(m_clip_mask_value + 1, 255));
        break;
    }
    addDraw(*reinterpret_cast<Geometry*>(geometry), kind, nullptr, translation, nullptr);
}

void UIRenderInterface::SetTransform(const Rml::Matrix4f* transform)
{
    if (!transform || !m_draw_data) {
        m_transform = -1;
        return;
    }
    glm::mat4 matrix{};
    std::memcpy(&matrix, transform->data(), sizeof(matrix)); // (both column major)
    m_transform = static_cast<int32_t>(m_draw_data->getTransforms().size());
    m_draw_data->getTransforms().push_back(matrix);
}

Rml::CompiledShaderHandle UIRenderInterface::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    // Only gradients. Their parameters are as in RmlUi's OpenGL 3 renderer
    UIGradient gradient{};
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if (name == "linear-gradient") {
        gradient.function = 0;
        const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        gradient.p = glm::vec2(p0.x, p0.y);
        const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
        gradient.v = glm::vec2(p1.x, p1.y) - gradient.p;
    }
    else if (name == "radial-gradient") {
        gradient.function = 1;
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        gradient.p = glm::vec2(center.x, center.y);
        gradient.v = glm::vec2(1.0f / std::max(radius.x, 1e-6f), 1.0f / std::max(radius.y, 1e-6f));
    }
    else if (name == "conic-gradient") {
        gradient.function = 2;
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        gradient.p = glm::vec2(center.x, center.y);
        gradient.v = glm::vec2(std::cos(angle), std::sin(angle));
    }
    else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader '%s'", name.c_str());
        return {};
    }
    if (repeating) {
        gradient.function += 3;
    }

    const auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) {
        return {};
    }
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    if (stops.empty()) {
        return {};
    }

    gradient.first_stop = stops.front().position.number;
    gradient.last_stop = stops.back().position.number;
    gradient.ramp_start = std::min(0.0f, gradient.first_stop);
    gradient.ramp_end = std::max(1.0f, gradient.last_stop);
    if (repeating) {
        gradient.ramp_start = gradient.first_stop;
        gradient.ramp_end = std::max(gradient.last_stop, gradient.first_stop + 1e-6f);
    }

    // The colors along the ramp, blended straight from one stop to the next as CSS does (alpha is already premultiplied)
    std::vector<uint8_t> ramp(GRADIENT_RAMP_SIZE * 4);
    for (uint32_t i = 0; i < GRADIENT_RAMP_SIZE; ++i) {
        const float t = gradient.ramp_start + (gradient.ramp_end - gradient.ramp_start) * static_cast<float>(i) / static_cast<float>(GRADIENT_RAMP_SIZE - 1);
        std::array<float, 4> color{};
        const auto setColor = [&color](const Rml::ColourbPremultiplied& c) {
            color = {static_cast<float>(c.red), static_cast<float>(c.green), static_cast<float>(c.blue), static_cast<float>(c.alpha)};
        };
        setColor(stops.front().color);
        for (size_t s = 1; s < stops.size(); ++s) {
            const float a = stops[s - 1].position.number;
            const float b = stops[s].position.number;
            if (t <= a) {
                break;
            }
            if (t >= b) {
                setColor(stops[s].color);
                continue;
            }
            const float f = (t - a) / (b - a);
            const Rml::ColourbPremultiplied& c0 = stops[s - 1].color;
            const Rml::ColourbPremultiplied& c1 = stops[s].color;
            color = {static_cast<float>(c0.red) + (static_cast<float>(c1.red) - static_cast<float>(c0.red)) * f,
                     static_cast<float>(c0.green) + (static_cast<float>(c1.green) - static_cast<float>(c0.green)) * f,
                     static_cast<float>(c0.blue) + (static_cast<float>(c1.blue) - static_cast<float>(c0.blue)) * f,
                     static_cast<float>(c0.alpha) + (static_cast<float>(c1.alpha) - static_cast<float>(c0.alpha)) * f};
            break;
        }
        for (size_t c = 0; c < 4; ++c) {
            ramp[i * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(color[c]), 0L, 255L));
        }
    }

    auto* const shader = new Shader{};
    shader->gradient = gradient;
    shader->ramp = m_render_backend.createUITexture(ramp, GRADIENT_RAMP_SIZE, 1, false);
    return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void UIRenderInterface::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                     Rml::TextureHandle /*texture*/)
{
    Shader& s = *reinterpret_cast<Shader*>(shader);
    addDraw(*reinterpret_cast<Geometry*>(geometry), UIDrawKind::GRADIENT, s.ramp.get(), translation, &s.gradient);
}

void UIRenderInterface::ReleaseShader(Rml::CompiledShaderHandle shader)
{
    Shader* const s = reinterpret_cast<Shader*>(shader);
    m_released_textures.push_back(std::move(s->ramp));
    delete s;
}

void UIRenderInterface::addDraw(Geometry& geometry, UIDrawKind kind, RenderUITexture* texture, Rml::Vector2f translation, const UIGradient* gradient)
{
    if (!m_draw_data || geometry.indices.empty()) {
        return;
    }

    // The geometry is copied in the first time it is drawn in the frame (a clip mask is often drawn with the same geometry as a box)
    if (geometry.frame != m_frame) {
        auto& vertices = m_draw_data->getVertices();
        auto& indices = m_draw_data->getIndices();
        geometry.frame = m_frame;
        geometry.vertex_offset = static_cast<int32_t>(vertices.size());
        geometry.first_index = static_cast<uint32_t>(indices.size());
        vertices.insert(vertices.end(), geometry.vertices.begin(), geometry.vertices.end());
        indices.insert(indices.end(), geometry.indices.begin(), geometry.indices.end());
    }

    UIDrawCommand command{};
    command.first_index = geometry.first_index;
    command.index_count = static_cast<uint32_t>(geometry.indices.size());
    command.vertex_offset = geometry.vertex_offset;
    command.kind = kind;
    command.use_clip_mask = m_clip_mask_enabled;
    command.clip_mask_value = m_clip_mask_value;
    command.texture = texture;
    command.translation = glm::vec2(translation.x, translation.y);
    command.transform = m_transform;
    command.scissor = m_scissor_enabled ? m_scissor : glm::ivec4(0, 0, m_size.x, m_size.y);
    if (gradient) {
        command.gradient = *gradient;
    }
    m_draw_data->getCommands().push_back(command);
}

} // namespace gc
