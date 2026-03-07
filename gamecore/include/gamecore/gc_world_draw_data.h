#pragma once

#include <vector>
#include <span>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec
class RenderMesh;     // forward-dec
class RenderTexture;  // forward-dec

struct WorldDrawEntry {
    glm::mat4 world_matrix;
    RenderMesh* mesh;
    RenderMaterial* material;
};

struct WorldInstancedDrawEntry {
    uint32_t transform_offset; // start index in m_draw_instance_transforms
    uint32_t instance_count;
    RenderMesh* mesh;
    RenderMaterial* material;
};

class WorldDrawData {
    std::vector<WorldDrawEntry> m_draw_entries{};

    std::vector<WorldInstancedDrawEntry> m_instanced_draw_entries{};
    std::vector<glm::mat4> m_instanced_draw_transforms{};

    glm::mat4 m_projection_matrix{};
    glm::mat4 m_view_matrix{};
    glm::vec3 m_light_pos{};

public:
    void reset()
    {
        m_draw_entries.clear();
        m_instanced_draw_entries.clear();
        m_instanced_draw_transforms.clear();
    }

    void drawMesh(const glm::mat4& world_matrix, RenderMesh* const mesh, RenderMaterial* const material)
    {
        m_draw_entries.emplace_back(world_matrix, mesh, material);
    }
    void drawMeshInstanced(const std::span<const glm::mat4> transforms, RenderMesh* const mesh, RenderMaterial* const material)
    {
        const auto transform_offset = static_cast<uint32_t>(m_instanced_draw_transforms.size());
        const auto instance_count = static_cast<uint32_t>(transforms.size());
        m_instanced_draw_entries.emplace_back(transform_offset, instance_count, mesh, material);
        m_instanced_draw_transforms.insert(m_instanced_draw_transforms.end(), transforms.begin(), transforms.end());
    }

    void setProjectionMatrix(const glm::mat4& projection_matrix) { m_projection_matrix = projection_matrix; }
    void setViewMatrix(const glm::mat4& view_matrix) { m_view_matrix = view_matrix; }
    void setLightPos(const glm::vec3& light_pos) { m_light_pos = light_pos; }

    const auto& getDrawEntries() const { return m_draw_entries; }
    const auto& getInstancedDrawEntries() const { return m_instanced_draw_entries; }
    const auto& getInstancedDrawTransforms() const { return m_instanced_draw_transforms; }

    const auto& getProjectionMatrix() const { return m_projection_matrix; }
    const auto& getViewMatrix() const { return m_view_matrix; }
    const auto& getLightPos() const { return m_light_pos; }
};

} // namespace gc
