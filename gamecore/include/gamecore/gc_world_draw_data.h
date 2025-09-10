#pragma once

#include <vector>

#include <mat4x4.hpp>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_gpu_resources.h"

namespace gc {

// Holds the draw data for the World this current frame.
// Owned by the RenderBackend, one per frame in flight.
// Every frame the draw data must be 'reset'.
class WorldDrawData {
    std::vector<glm::mat4> m_cube_matrices{}; // start simple
    GPUPipeline* m_pipeline{};

public:
    void drawCube(const glm::mat4& model_matrix) { m_cube_matrices.push_back(model_matrix); }
    void setPipeline(GPUPipeline* pipeline) { m_pipeline = pipeline; }

    void reset() { m_cube_matrices.clear(); }
    const auto& getCubeMatrices() const { return m_cube_matrices; }
    auto getPipeline() const { return m_pipeline; }
};

} // namespace gc