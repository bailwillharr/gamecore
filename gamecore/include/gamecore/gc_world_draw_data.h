#pragma once

#include <vector>

#include <vec3.hpp>
#include <mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec
class RenderMesh;     // forward-dec

struct WorldDrawEntry {
    glm::mat4 world_matrix;
    RenderMesh* mesh;
    RenderMaterial* material;
};

class WorldDrawData {
    std::vector<WorldDrawEntry> m_draw_entries{};
    glm::vec3 m_light_pos{};

public:
    void drawMesh(const glm::mat4& world_matrix, RenderMesh* mesh, RenderMaterial* material) { m_draw_entries.emplace_back(world_matrix, mesh, material); }
    void setLightPos(const glm::vec3& light_pos) { m_light_pos = light_pos; }
    void reset() { m_draw_entries.clear(); }

    const auto& getDrawEntries() const { return m_draw_entries; }
    const auto& getLightPos() const { return m_light_pos; }
};

} // namespace gc
