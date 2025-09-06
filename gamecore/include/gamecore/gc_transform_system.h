#pragma once

#include <vector>
#include <unordered_map>
#include <span>

#include <mat4x4.hpp>

#include "gamecore/gc_ecs.h"

namespace gc {

class World; // forward-dec

class TransformSystem : public System {

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

    /* gets a non-owning list of children of an entity, only guaranteed to be valid until the TransformSystem is next updated */
    std::span<const Entity> getChildren(Entity parent) const
    {
        if (auto it = m_parent_children.find(parent); it != m_parent_children.end()) {
            return it->second;
        }
        else {
            return {};
        }
    }

private:
    void updateWorldMatricesRecursively(Entity entity, const glm::mat4& parent_matrix = glm::mat4{1.0f});
};

} // namespace gc