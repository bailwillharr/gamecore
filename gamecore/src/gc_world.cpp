#include "gamecore/gc_world.h"

#include <tracy/Tracy.hpp>

#include "gclog/gclog.h"
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
        entity = m_free_entity_ids.back();
        m_free_entity_ids.pop_back();
        m_entity_signatures[entity] = Signature{};
    }

    TransformComponent& t = addComponent<TransformComponent>(entity);
    t.name = name;
    t.setPosition(position);
    t.setRotation(rotation);
    t.setScale(scale);

    getSystem<TransformSystem>().setParent(entity, parent);

    m_max_alive_entity_id = std::max(m_max_alive_entity_id, entity);

    return entity;
}

void World::deleteEntity(const Entity entity)
{
    GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));
    GC_ASSERT(m_entity_signatures[entity].hasTypes<TransformComponent>());

    auto& transform_system = getSystem<TransformSystem>();

    // delete children:
    // (a copy, as deleting a child removes it from the list that getChildren() refers to)
    const auto children_span = transform_system.getChildren(entity);
    const std::vector<Entity> children(children_span.begin(), children_span.end());
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
    m_free_entity_ids.push_back(entity);

    if (m_max_alive_entity_id == entity) {
        --m_max_alive_entity_id;
    }
}

Entity World::findEntity(const Name name)
{
    // TODO: this is still a linear search.
    // Perhaps cache name->entity mappings
    for (Entity entity = 0; entity <= m_max_alive_entity_id && entity < m_entity_signatures.size(); ++entity) {
        // erased entities have an empty signature so getComponent() returns nullptr for them.
        if (const TransformComponent* t = getComponent<TransformComponent>(entity); t && t->name == name) {
            return entity;
        }
    }
    return ENTITY_NONE;
}

void World::update(FrameState& frame_state)
{
    ZoneScoped;
    for (auto& system : m_systems) {
        system->onUpdate(frame_state);
    }
}

bool World::isEntityAlive(Entity entity) const
{
    // every entity has a TransformComponent, so a living entity's signature is never empty
    return entity < m_entity_signatures.size() && m_entity_signatures[entity].componentCount() > 0;
}

std::vector<Name> World::getComponentList(Entity entity) const
{
    std::vector<Name> list{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_arrays.size()); ++i) {
        if (m_entity_signatures[entity].hasComponentIndex(i)) {
            list.push_back(m_component_names[i]);
        }
    }
    return list;
}

bool World::isComponentRegistered(Name component_name) const { return findComponentIndex(component_name) != MAX_COMPONENTS; }

std::optional<size_t> World::getComponentSerialisedSize(Name component_name) const
{
    const uint32_t component_index = findComponentIndex(component_name);
    if (component_index == MAX_COMPONENTS || !m_component_arrays[component_index].serialise) {
        return {};
    }
    return m_component_arrays[component_index].serialised_size;
}

bool World::serialiseComponent(Entity entity, Name component_name, ByteWriter& writer)
{
    GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));

    const uint32_t component_index = findComponentIndex(component_name);
    if (component_index == MAX_COMPONENTS) {
        return false;
    }
    ComponentArrayEntry& entry = m_component_arrays[component_index];
    if (!entry.serialise || !m_entity_signatures[entity].hasComponentIndex(component_index)) {
        return false;
    }

    [[maybe_unused]] const size_t start = writer.pos();
    entry.serialise(entry.component_array->getRaw(entity), writer);
    GC_ASSERT(writer.pos() - start == entry.serialised_size);
    return true;
}

bool World::deserialiseComponent(Entity entity, Name component_name, ByteReader& reader)
{
    GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));

    const uint32_t component_index = findComponentIndex(component_name);
    if (component_index == MAX_COMPONENTS) {
        return false;
    }
    ComponentArrayEntry& entry = m_component_arrays[component_index];
    if (!entry.deserialise) {
        return false;
    }

    // The pointer is to a real object of the component's type, so there's nothing to worry about with alignment or aliasing.
    void* component{};
    if (m_entity_signatures[entity].hasComponentIndex(component_index)) {
        component = entry.component_array->getRaw(entity);
    }
    else {
        m_entity_signatures[entity].setWithIndex(component_index);
        component = entry.component_array->addComponent(entity);
    }

    [[maybe_unused]] const size_t start = reader.pos();
    entry.deserialise(reader, component);
    GC_ASSERT(reader.pos() - start == entry.serialised_size);
    return true;
}

uint32_t World::findComponentIndex(Name component_name) const
{
    // there are at most MAX_COMPONENTS names to look through
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_component_names.size()); ++i) {
        if (m_component_names[i] == component_name) {
            return i;
        }
    }
    return static_cast<uint32_t>(MAX_COMPONENTS);
}

} // namespace gc
