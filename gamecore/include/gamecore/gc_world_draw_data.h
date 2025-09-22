#pragma once

#include <vector>

#include <mat4x4.hpp>

#include "gamecore/gc_gpu_resources.h"

namespace gc {

// Holds the draw data for the World this current frame.
// Owned by the RenderBackend, one per frame in flight.
// Every frame the draw data must be 'reset'.
class WorldDrawData {
    std::vector<glm::mat4> m_cube_matrices{}; // start simple
    GPUPipeline* m_pipeline{};
    GPUImageView* m_texture{};

public:
    void drawCube(const glm::mat4& model_matrix) { m_cube_matrices.push_back(model_matrix); }
    void reset() { m_cube_matrices.clear(); }

    void setPipeline(GPUPipeline* pipeline) { m_pipeline = pipeline; }
    void setTexture(GPUImageView* texture) { m_texture = texture; }

    const auto& getCubeMatrices() const { return m_cube_matrices; }
    auto getPipeline() const { return m_pipeline; }
    auto getTexture() const { return m_texture; }
};

} // namespace gc
