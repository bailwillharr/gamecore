#pragma once

#include <vector>

#include <vec3.hpp>

#include "gamecore/gc_vulkan_common.h"

namespace gc {

// Holds the draw data for the World this current frame.
// Owned by the RenderBackend, one per frame in flight.
// Every frame the draw data must be 'reset'.
class WorldDrawData {
    std::vector<glm::vec3> m_triangle_positions{}; // start simple

public:
    void drawTriangle(glm::vec3 position) { m_triangle_positions.push_back(position); }

    void reset() { m_triangle_positions.clear(); }
    const std::vector<glm::vec3>& getTrianglePositions() const { return m_triangle_positions; }
};

} // namespace gc