#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_frame_state.h"

#include <vector>
#include <memory>
#include <optional>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

/* The World contains all loaded entities in the game. */

namespace gc {

class World {
    struct ComponentArrayEntry {
        std::unique_ptr<IComponentArray> component_array;
        ComponentArrayType type;
        // Type erased T::serialise() and T::deserialise(). Null if the component isn't a SerialisableComponent.
        void (*serialise)(const void* component, ByteWriter& writer);
        void (*deserialise)(ByteReader& reader, void* component);
        size_t serialised_size;
    };

    std::vector<ComponentArrayEntry> m_component_arrays{};
    std::vector<Signature> m_entity_signatures{};
    std::vector<Entity> m_free_entity_ids;
    std::vector<std::unique_ptr<System>> m_systems{};

    std::vector<Name> m_component_names{};
    std::vector<Name> m_system_names{};

    Entity m_max_alive_entity_id{};

public:
    World();
    World(const World&) = delete;

    ~World();

    World& operator=(const World&) = delete;

    void update(FrameState& frame_state);

    Entity createEntity(Name name, Entity parent = ENTITY_NONE, const glm::vec3& position = glm::vec3{0.0f, 0.0f, 0.0f},
                        const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, const glm::vec3& scale = glm::vec3{1.0f, 1.0f, 1.0f});

    void deleteEntity(Entity entity);

    // returns ENTITY_NONE on failure
    Entity findEntity(Name name);

    // Create a ComponentArray for the given component
    template <ValidComponent T, ComponentArrayType ArrayType>
    void registerComponent()
    {
        const uint32_t component_index = getComponentIndex<T>();
        if (component_index != m_component_arrays.size()) {
            gc::abortGame("Attempt to register same component twice!");
        }
        if (isComponentRegistered(T::NAME)) {
            // components are looked up by name when loading prefabs
            gc::abortGame("Attempt to register two components with the same name: {}", Name(T::NAME));
        }
        ComponentArrayEntry entry{};
        entry.component_array = std::make_unique<ComponentArray<T, ArrayType>>();
        entry.type = ArrayType;
        if constexpr (SerialisableComponent<T>) {
            entry.serialise = [](const void* component, ByteWriter& writer) { static_cast<const T*>(component)->serialise(writer); };
            entry.deserialise = [](ByteReader& reader, void* component) { static_cast<T*>(component)->deserialise(reader); };
            entry.serialised_size = T::getSerialisedSize();
        }
        m_component_arrays.push_back(std::move(entry));
        m_component_names.push_back(T::NAME);
    }

    // The returned reference can be invalidated when addComponent() is called again for the same component type.
    template <ValidComponent T>
    T& addComponent(const Entity entity)
    {
        GC_ASSERT(entity != ENTITY_NONE);

        const uint32_t component_index = getComponentIndex<T>();

        GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));
        GC_ASSERT(!m_entity_signatures[entity].hasComponentIndex(component_index) && "Component already exists!");

        m_entity_signatures[entity].setWithIndex(component_index);

        GC_ASSERT(component_index < static_cast<uint32_t>(m_component_arrays.size()));
        GC_ASSERT(m_component_arrays[component_index].component_array);

        return *static_cast<T*>(m_component_arrays[component_index].component_array->addComponent(entity));
    }

    template <ValidComponent T>
    void removeComponent(const Entity entity)
    {
        GC_ASSERT(entity != ENTITY_NONE);

        const uint32_t component_index = getComponentIndex<T>();

        GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));
        GC_ASSERT(m_entity_signatures[entity].hasComponentIndex(component_index) &&
                  "Attempt to remove component from entity. But component didn't exist in the first place!");

        m_entity_signatures[entity].setWithIndex(component_index, false);

        GC_ASSERT(component_index < static_cast<uint32_t>(m_component_arrays.size()));
        GC_ASSERT(m_component_arrays[component_index].component_array);

        m_component_arrays[component_index].component_array->removeComponent(entity);
    }

    // returns nullptr if component does not exist in entity
    template <ValidComponent T>
    T* getComponent(const Entity entity)
    {
        if (entity == ENTITY_NONE) {
            return nullptr;
        }

        const uint32_t component_index = getComponentIndex<T>();

        GC_ASSERT(entity < static_cast<uint32_t>(m_entity_signatures.size()));

        if (!m_entity_signatures[entity].hasComponentIndex(component_index)) {
            return nullptr;
        }
        else {
            GC_ASSERT(component_index < static_cast<uint32_t>(m_component_arrays.size()));
            GC_ASSERT(m_component_arrays[component_index].component_array);

            auto& component_array_entry = m_component_arrays[component_index];
            if (component_array_entry.type == ComponentArrayType::SPARSE) {
                auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::SPARSE>&>(*(component_array_entry.component_array));
                return &component_array.get(entity);
            }
            else {
                auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::DENSE>&>(*(component_array_entry.component_array));
                return &component_array.get(entity);
            }
        }
    }

    std::vector<Name> getComponentList(Entity entity) const;

    // The functions below work on components by name, without knowing their type. This is how prefabs are loaded and saved.

    bool isComponentRegistered(Name component_name) const;

    // Returns the number of bytes that serialiseComponent() writes and deserialiseComponent() reads.
    // Empty if the component isn't registered or isn't a SerialisableComponent.
    std::optional<size_t> getComponentSerialisedSize(Name component_name) const;

    // Returns false, writing nothing, if the component isn't serialisable or the entity doesn't have it.
    bool serialiseComponent(Entity entity, Name component_name, ByteWriter& writer);

    // Adds the component to the entity if it doesn't have it yet, then sets it from the reader.
    // Returns false, reading nothing, if the component isn't registered or isn't serialisable.
    bool deserialiseComponent(Entity entity, Name component_name, ByteReader& reader);

    template <ValidDerivedSystem T, typename... Args>
    void registerSystem(Args&&... args)
    {
        const uint32_t system_index = getSystemIndex<T>();
        if (system_index != m_systems.size()) {
            gc::abortGame("Attempt to register same system twice!");
        }
        m_systems.push_back(std::make_unique<T>(*this, std::forward<Args>(args)...));
        m_system_names.push_back(T::NAME);
    }

    template <ValidDerivedSystem T>
    T& getSystem()
    {
        const uint32_t system_index = getSystemIndex<T>();
        GC_ASSERT(system_index < m_systems.size());
        GC_ASSERT(m_systems[system_index]);
        return static_cast<T&>(*m_systems[system_index]);
    }

    template <ValidComponent... Ts, typename Func>
    void forEach(Func&& func)
    {
        for (Entity entity = 0; entity <= m_max_alive_entity_id && entity < m_entity_signatures.size(); ++entity) {
            // erased entities will have an empty signature so will be skipped over here.
            if (m_entity_signatures[entity].hasTypes<Ts...>()) {
                auto components = std::make_tuple(getComponent<Ts>(entity)...);
                GC_ASSERT((... && std::get<Ts*>(components)));
                std::apply([&](Ts*... comps) { func(entity, *comps...); }, components);
            }
        }
    }

private:
    // returns MAX_COMPONENTS if no component with that name is registered
    uint32_t findComponentIndex(Name component_name) const;
};

} // namespace gc
