#pragma once

#include <vector>
#include <unordered_map>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_core_components.h"

namespace gc {

class World; // forward-dec

class TransformSystem : public System {
    std::unordered_map<Entity, std::vector<Entity>> m_children{};

public:
    TransformSystem(gc::World& world);

    void onUpdate(float ts) override;

    /*
     * entity must be a valid entity.
     * parent can be ENTITY_NONE.
     */
    void setParent(Entity entity, Entity parent);
};

} // namespace gc