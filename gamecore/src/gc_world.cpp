#include "gamecore/gc_world.h"

#include "gamecore/gc_logger.h"
#include "gamecore/gc_core_components.h"
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
    t.position = position;
    t.rotation = rotation;
    t.scale = scale;

    getSystem<TransformSystem>().setParent(entity, parent);

    return entity;
}

bool World::tryDeleteEntity(const Entity entity)
{
    // TODO 1: Delete all the entity's components.
    // TODO 2: Delete the entity's children recursively
    GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));
    GC_ASSERT(m_entity_signatures[entity].hasTypes<TransformComponent>());

    const uint32_t transform_component_index = getComponentIndex<TransformComponent>();

    GC_ASSERT(transform_component_index < static_cast<uint32_t>(m_component_arrays.size()));
    GC_ASSERT(m_component_arrays[transform_component_index].component_array);

    if (m_entity_signatures[entity].componentCount() == 1) {
        m_component_arrays[transform_component_index].component_array->removeComponent(entity);
        return true;
    }
    else {
        return false;
    }
}

void World::update(float ts)
{
    for (auto& system : m_systems) {
         system->onUpdate(ts);
    }
}

} // namespace gc