#include "gamecore/gc_resource_manager.h"

#include <atomic>

namespace gc {

std::atomic<uint32_t> g_next_resource_index{};

} // namespace gc