#pragma once

#include <span>

#include "gamecore/gc_ecs.h"

namespace gc {

class World; // forward-dec

// returns the number of loaded entities
Entity loadPrefab(std::span<const uint8_t> data, World& world, Entity prefab_parent = ENTITY_NONE);

} // namespace gc