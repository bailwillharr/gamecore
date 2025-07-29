#include "gamecore/gc_transform_system.h"

#include <algorithm>

#include "gamecore/gc_core_components.h"
#include "gamecore/gc_world.h"

namespace gc {

TransformSystem::TransformSystem(gc::World& world) : gc::System(world, Signature::fromTypes<TransformComponent>()) {}

void TransformSystem::onUpdate(float ts)
{
    (void)ts;

    /*
    for (gc::Entity entity : m_entities) {
        gc::TransformComponent* t = m_world.getComponent<gc::TransformComponent>(entity);
        GC_ASSERT(t);

        glm::mat4 transform = glm::mat4_cast(t->rotation);
        transform[3][0] = t->position.x;
        transform[3][1] = t->position.y;
        transform[3][2] = t->position.z;
        transform = glm::scale(transform, t->scale);

        if (t->parent != gc::ENTITY_NONE) {
            const gc::TransformComponent* parent_t = m_world.getComponent<gc::TransformComponent>(t->parent);
            GC_ASSERT(parent_t);
            transform = parent_t->world_matrix * transform;
        }

        t->world_matrix = transform;
    }
    */
}

void TransformSystem::setParent(Entity entity, Entity parent)
{
    TransformComponent* entity_transform = m_world.getComponent<TransformComponent>(entity);
    GC_ASSERT(entity_transform);

    if (entity_transform->parent != ENTITY_NONE) {
        GC_ASSERT(m_children.contains(entity_transform->parent));
        std::vector<Entity>& previous_parents_children = m_children[entity_transform->parent];
        auto it = std::find(previous_parents_children.cbegin(), previous_parents_children.cend(), entity);
        GC_ASSERT(it != previous_parents_children.cend());
        previous_parents_children.erase(it);
    }

    if (parent) {
        if (m_children.contains(parent)) {
            m_children[parent].push_back(entity);
        }
        else {
            m_children.insert({parent, {entity}});
        }
    }

    entity_transform->parent = parent;
}

} // namespace gc