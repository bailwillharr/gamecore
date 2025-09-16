#include "gamecore/gc_ecs.h"

#include <atomic>

namespace gc {

std::atomic<uint32_t> g_next_component_index{};
std::atomic<uint32_t> g_next_system_index{};
std::atomic<uint32_t> g_next_frame_state_object_index{};

System::System(World& world) : m_world(world) {}

} // namespace gc
