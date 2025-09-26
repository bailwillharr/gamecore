#pragma once

#include <vector>

#include <mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec

// Holds the draw data for the World this current frame.
// Owned by the RenderBackend, one per frame in flight.
// Every frame the draw data must be 'reset'.
class WorldDrawData {
    std::vector<glm::mat4> m_cube_matrices{}; // start simple
    RenderMaterial* m_material{};

public:
    void drawCube(const glm::mat4& model_matrix) { m_cube_matrices.push_back(model_matrix); }
    void reset() { m_cube_matrices.clear(); }

    void setMaterial(RenderMaterial* material) { m_material = material; }

    const auto& getCubeMatrices() const { return m_cube_matrices; }
    auto getMaterial() const { return m_material; }
};

} // namespace gc
