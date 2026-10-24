#pragma once

// What the game's UI (see gc_ui.h) gives the RenderBackend to draw each frame, as WorldDrawData is for the world.
// The UI is drawn over the world, before ImGui, in the same render pass (so that it is antialiased like the world).

#include <cstdint>

#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace gc {

class RenderUITexture; // forward-dec

// The same layout as RmlUi's vertices (Rml::Vertex), so that its geometry can be copied straight in
struct UIVertex {
    glm::vec2 position; // in pixels, from the top left of the screen
    uint32_t color;     // RGBA8, sRGB, alpha premultiplied
    glm::vec2 uv;
};
static_assert(sizeof(UIVertex) == 20);

enum class UIDrawKind : uint8_t {
    TRIANGLES,        // vertex colors, times the texture if there is one
    GRADIENT,         // vertex colors, times a gradient (see UIGradient). The texture is the gradient's colors
    CLIP_SET,         // the clip mask becomes the area that the triangles cover. Nothing is drawn. See Rml::ClipMaskOperation
    CLIP_SET_INVERSE, // the clip mask becomes the area that the triangles don't cover
    CLIP_INTERSECT,   // the clip mask becomes the part of itself that the triangles cover
};

// Where a gradient is along each pixel. The texture of a GRADIENT draw holds its colors, from ramp_start to ramp_end.
struct UIGradient {
    uint32_t function; // 0: linear, 1: radial, 2: conic. 3, 4 and 5 are the same, repeating
    glm::vec2 p;       // linear: the starting point. Radial and conic: the centre (all in the vertices' uv space)
    glm::vec2 v;       // linear: from the starting point to the end. Radial: 1 / the radius. Conic: the unit vector of its angle
    float first_stop;  // where a repeating gradient repeats from and to (0 is the start, 1 the end)
    float last_stop;
    float ramp_start;  // where the texture's first and last texels are
    float ramp_end;
};

struct UIDrawCommand {
    uint32_t first_index;
    uint32_t index_count;
    int32_t vertex_offset;
    UIDrawKind kind;
    // TRIANGLES and GRADIENT: only draw where the clip mask is clip_mask_value. CLIP_INTERSECT: the value the mask has now
    bool use_clip_mask;
    uint8_t clip_mask_value;
    RenderUITexture* texture; // null for none. Never null for a GRADIENT
    glm::vec2 translation;    // added to the vertices' positions, before the transform
    int32_t transform;        // index into UIDrawData::getTransforms(), or -1 for none
    glm::ivec4 scissor;       // x, y, width, height, in pixels
    UIGradient gradient;
};

class UIDrawData {
    std::vector<UIVertex> m_vertices{};
    std::vector<uint32_t> m_indices{};
    std::vector<UIDrawCommand> m_commands{};
    std::vector<glm::mat4> m_transforms{};
    glm::ivec2 m_size{}; // of the screen that the UI was laid out on, in pixels

public:
    void reset()
    {
        m_vertices.clear();
        m_indices.clear();
        m_commands.clear();
        m_transforms.clear();
    }

    bool empty() const { return m_commands.empty(); }

    void setSize(glm::ivec2 size) { m_size = size; }
    glm::ivec2 getSize() const { return m_size; }

    std::vector<UIVertex>& getVertices() { return m_vertices; }
    const std::vector<UIVertex>& getVertices() const { return m_vertices; }
    std::vector<uint32_t>& getIndices() { return m_indices; }
    const std::vector<uint32_t>& getIndices() const { return m_indices; }
    std::vector<UIDrawCommand>& getCommands() { return m_commands; }
    const std::vector<UIDrawCommand>& getCommands() const { return m_commands; }
    std::vector<glm::mat4>& getTransforms() { return m_transforms; }
    const std::vector<glm::mat4>& getTransforms() const { return m_transforms; }
};

} // namespace gc
