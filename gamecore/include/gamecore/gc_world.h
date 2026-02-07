#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_name.h"
#include "gamecore/gc_frame_state.h"

#include <vector>
#include <memory>
#include <stack>

#include <vec3.hpp>
#include <ext/quaternion_float.hpp>

/* The World contains all loaded entities in the game. */

namespace gc {

class World {
    struct ComponentArrayEntry {
        std::unique_ptr<IComponentArray> component_array;
        ComponentArrayType type;
    };

    std::vector<ComponentArrayEntry> m_component_arrays{};
    std::vector<Signature> m_entity_signatures{};
    std::stack<Entity> m_free_entity_ids;
    std::vector<std::unique_ptr<System>> m_systems{};

public:
    World();
    World(const World&) = delete;

    ~World();

    World& operator=(const World&) = delete;

    void update(FrameState& frame_state);

    Entity createEntity(Name name, Entity parent = ENTITY_NONE, const glm::vec3& position = glm::vec3{0.0f, 0.0f, 0.0f},
                        const glm::quat& rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, const glm::vec3& scale = glm::vec3{1.0f, 1.0f, 1.0f});

    // This function will only succeed when the only remaining component is the TransformComponent
    void deleteEntity(Entity entity);

    // Create a ComponentArray for the given component
    template <ValidComponent T, ComponentArrayType ArrayType>
    void registerComponent()
    {
        const uint32_t component_index = getComponentIndex<T>();
        if (component_index != m_component_arrays.size()) {
            gc::abortGame("Attempt to register same component twice!");
        }
        m_component_arrays.emplace_back(std::make_unique<ComponentArray<T, ArrayType>>(), ArrayType);
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

        m_component_arrays[component_index].component_array->addComponent(entity);

        if (m_component_arrays[component_index].type == ComponentArrayType::SPARSE) {
            auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::SPARSE>&>(*(m_component_arrays[component_index].component_array));
            return component_array.get(entity);
        }
        else {
            auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::DENSE>&>(*(m_component_arrays[component_index].component_array));
            return component_array.get(entity);
        }
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

            if (m_component_arrays[component_index].type == ComponentArrayType::SPARSE) {
                auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::SPARSE>&>(*(m_component_arrays[component_index].component_array));
                return &component_array.get(entity);
            }
            else {
                auto& component_array = static_cast<ComponentArray<T, ComponentArrayType::DENSE>&>(*(m_component_arrays[component_index].component_array));
                return &component_array.get(entity);
            }
        }
    }

    template <ValidDerivedSystem T, typename... Args>
    void registerSystem(Args&&... args)
    {
        const uint32_t system_index = getSystemIndex<T>();
        if (system_index != m_systems.size()) {
            gc::abortGame("Attempt to register same system twice!");
        }
        m_systems.push_back(std::make_unique<T>(*this, std::forward<Args>(args)...));
    }

    template <ValidDerivedSystem T>
    T& getSystem()
    {
        const uint32_t system_index = getSystemIndex<T>();
        GC_ASSERT(system_index < m_systems.size());
        return static_cast<T&>(*m_systems[system_index]);
    }

    template <ValidComponent... Ts, typename Func>
    void forEach(Func&& func)
    {
        for (Entity entity = 0; entity < m_entity_signatures.size(); ++entity) {
            // erased entities will have an empty signature so will be skipped over here.
            if (m_entity_signatures[entity].hasTypes<Ts...>()) {
                auto components = std::make_tuple(getComponent<Ts>(entity)...);
                GC_ASSERT((... && std::get<Ts*>(components)));
                std::apply([&](Ts*... comps) { func(entity, *comps...); }, components);
            }
        }
    }
};

} // namespace gc
