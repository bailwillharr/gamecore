#include "gamecore/gc_world.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_transform_system.h"

namespace gc {

World::World()
{
    registerComponent<TransformComponent, ComponentArrayType::DENSE>();
    registerSystem<TransformSystem>();

    GC_TRACE("Initialised World");
}

World::~World() { GC_TRACE("Destroying World..."); }

Entity World::createEntity(Name name, Entity parent, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
{
    Entity entity{};
    if (m_free_entity_ids.empty()) {
        entity = static_cast<uint32_t>(m_entity_signatures.size());
        m_entity_signatures.resize(m_entity_signatures.size() + 1);
    }
    else {
        entity = m_free_entity_ids.top();
        m_free_entity_ids.pop();
        m_entity_signatures[entity] = Signature{};
    }

    TransformComponent& t = addComponent<TransformComponent>(entity);
    t.name = name;
    t.setPosition(position);
    t.setRotation(rotation);
    t.setScale(scale);

    getSystem<TransformSystem>().setParent(entity, parent);

    return entity;
}

void World::deleteEntity(const Entity entity)
{
    GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));
    GC_ASSERT(m_entity_signatures[entity].hasTypes<TransformComponent>());

    auto& transform_system = getSystem<TransformSystem>();

    // delete children:
    auto children = transform_system.getChildren(entity);
    for (Entity child : children) {
        deleteEntity(child);
    }

    // remove from TransformSystem's m_parent_children map
    transform_system.setParent(entity, ENTITY_NONE);

    // delete all components
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_arrays.size()); ++i) {
        if (m_entity_signatures[entity].hasComponentIndex(i)) {
            m_component_arrays[i].component_array->removeComponent(entity);
        }
    }

    m_entity_signatures[entity] = Signature{}; // an empty signature in m_entity_signatures means no entity
    m_free_entity_ids.push(entity);
}

void World::update(FrameState& frame_state)
{
    ZoneScoped;
    for (auto& system : m_systems) {
        system->onUpdate(frame_state);
    }
}

} // namespace gc
