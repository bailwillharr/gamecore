#include "gamecore/gc_transform_system.h"

#include <algorithm>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"
#include "gamecore/gc_frame_state.h"

namespace gc {

TransformSystem::TransformSystem(gc::World& world) : gc::System(world) {}

void TransformSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    (void)frame_state;

    m_world.forEach<TransformComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t) {
        if (t.m_dirty) {
            // t.m_dirty is reset by updateWorldMatricesRecursively()
            if (t.m_parent == ENTITY_NONE) {
                updateWorldMatricesRecursively(entity);
            }
            else {
                updateWorldMatricesRecursively(entity, m_world.getComponent<TransformComponent>(t.m_parent)->m_world_matrix);
            }
        }
    });
}

void TransformSystem::setParent(Entity entity, Entity parent)
{
    TransformComponent* entity_transform = m_world.getComponent<TransformComponent>(entity);
    GC_ASSERT(entity_transform);

    if (entity_transform->m_parent != ENTITY_NONE) {
        // remove entity from old parent's children array
        GC_ASSERT(m_parent_children.contains(entity_transform->m_parent));
        auto& previous_parents_children = m_parent_children[entity_transform->m_parent];
        auto it = std::find(previous_parents_children.cbegin(), previous_parents_children.cend(), entity);
        GC_ASSERT(it != previous_parents_children.cend());
        previous_parents_children.erase(it);
    }

    if (parent != ENTITY_NONE) {
        // add entity to new parent's children array
        if (auto it = m_parent_children.find(parent); it != m_parent_children.end()) {
            it->second.push_back(entity);
        }
        else {
            m_parent_children.insert({parent, {entity}});
        }
    }

    entity_transform->m_dirty = true;
    entity_transform->m_parent = parent;
}

void TransformSystem::updateWorldMatricesRecursively(const Entity entity, const glm::mat4& parent_matrix)
{
    // GC_TRACE("Updating world matrix for {}", entity);

    TransformComponent* t = m_world.getComponent<TransformComponent>(entity);
    GC_ASSERT(t);

    glm::mat4 local_matrix = glm::mat4_cast(t->m_rotation);
    local_matrix[3][0] = t->m_position.x;
    local_matrix[3][1] = t->m_position.y;
    local_matrix[3][2] = t->m_position.z;
    local_matrix = glm::scale(local_matrix, t->m_scale);
    t->m_world_matrix = parent_matrix * local_matrix;

    t->m_dirty = false;

    if (auto it = m_parent_children.find(entity); it != m_parent_children.end()) {
        for (Entity child : it->second) {
            updateWorldMatricesRecursively(child, t->m_world_matrix);
        }
    }
}

} // namespace gc
