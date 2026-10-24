#pragma once

#include <glm/vec3.hpp>

#include "gamecore/gc_world_draw_data.h"
#include "gamecore/gc_ui_draw_data.h"
#include "gamecore/gc_net.h"

namespace gc {

class WindowState; // forward-dec

// Data that is passed into systems
struct FrameState {
    const WindowState* window_state{};
    uint64_t frame_count{};
    double delta_time{};
    double average_frame_time{};
    WorldDrawData draw_data{};
    UIDrawData ui_draw_data{}; // written by the UI after the world's update
    std::vector<NetEvent> net_events{};
};

} // namespace gc
