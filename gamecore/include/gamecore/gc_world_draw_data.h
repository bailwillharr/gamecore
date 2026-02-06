#pragma once

#include <vector>

#include <vec3.hpp>
#include <mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec
class RenderMesh;     // forward-dec
class RenderTexture;  // forward-dec

struct WorldDrawEntry {
    glm::mat4 world_matrix;
    RenderMesh* mesh;
    RenderMaterial* material;
};

class WorldDrawData {
    std::vector<WorldDrawEntry> m_draw_entries{};
    RenderMaterial* m_fallback_material{};
    glm::mat4 m_projection_matrix{};
    glm::mat4 m_view_matrix{};
    glm::vec3 m_light_pos{};

public:
    void drawMesh(const glm::mat4& world_matrix, RenderMesh* mesh, RenderMaterial* material) { m_draw_entries.emplace_back(world_matrix, mesh, material); }
    void setFallbackMaterial(RenderMaterial* fallback_material) { m_fallback_material = fallback_material; }
    void setProjectionMatrix(const glm::mat4& projection_matrix) { m_projection_matrix = projection_matrix; }
    void setViewMatrix(const glm::mat4& view_matrix) { m_view_matrix = view_matrix; }
    void setLightPos(const glm::vec3& light_pos) { m_light_pos = light_pos; }
    void reset() { m_draw_entries.clear(); }

    const auto& getDrawEntries() const { return m_draw_entries; }
    auto getFallbackMaterial() const { return m_fallback_material; }
    const auto& getProjectionMatrix() const { return m_projection_matrix; }
    const auto& getViewMatrix() const { return m_view_matrix; }
    const auto& getLightPos() const { return m_light_pos; }
};

} // namespace gc
