#pragma once

#include <vector>

#include <mat4x4.hpp>

namespace gc {

class WindowState; // forward-dec

// Data that is passed into systems
struct FrameState {
    const WindowState* window_state{};
    double delta_time{};
    std::vector<glm::mat4> cube_transforms{};
};

} // namespace gc
