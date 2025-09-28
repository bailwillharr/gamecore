#pragma once

#include <vector>

#include <mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec
class RenderMesh;     // forward-dec

struct WorldDrawEntry {
    glm::mat4 world_matrix{};
    RenderMesh* mesh{};
};

class WorldDrawData {
    std::vector<WorldDrawEntry> m_draw_entries{}; // start simple
    RenderMaterial* m_material{};

public:
    void drawMesh(const glm::mat4& world_matrix, RenderMesh* mesh) { m_draw_entries.emplace_back(world_matrix, mesh); }
    void reset() { m_draw_entries.clear(); }

    void setMaterial(RenderMaterial* material) { m_material = material; }

    const auto& getDrawEntries() const { return m_draw_entries; }
    auto getMaterial() const { return m_material; }
};

} // namespace gc
