#pragma once

#include <vector>

#include <vec2.hpp>

#include "gamecore/gc_world_draw_data.h"

namespace gc {

class WindowState; // forward-dec

// Data that is passed into systems
struct FrameState {
    const WindowState* window_state{};
    uint64_t frame_count{};
    double delta_time{};
    double average_frame_time{};
    WorldDrawData draw_data{};
    glm::vec2 current_velocity{};
};

} // namespace gc
