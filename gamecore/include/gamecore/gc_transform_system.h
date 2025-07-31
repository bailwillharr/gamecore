#pragma once

#include <vector>
#include <unordered_map>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_core_components.h"

namespace gc {

class World; // forward-dec

class TransformSystem : public System {
    friend class World;

    // Entities could be used instead of TransformComponent pointers here. This just reduces the number of calls to getComponent<T>() in the update loop.
    std::unordered_map<Entity, std::vector<Entity>> m_parent_children{};

public:
    TransformSystem(gc::World& world);

    void onUpdate(double dt) override;

    /*
     * entity must be a valid entity.
     * parent can be ENTITY_NONE.
     */
    void setParent(Entity entity, Entity parent);

private:
    void updateWorldMatricesRecursively(Entity entity, const glm::mat4& parent_matrix = glm::mat4{1.0f});

};

} // namespace gc