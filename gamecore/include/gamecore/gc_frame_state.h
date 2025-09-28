#pragma once

#include <vector>

#include "gamecore/gc_world_draw_data.h"

namespace gc {

class WindowState; // forward-dec

// Data that is passed into systems
struct FrameState {
    const WindowState* window_state{};
    double delta_time{};
    double average_frame_time{};
    WorldDrawData draw_data{};
};

} // namespace gc
