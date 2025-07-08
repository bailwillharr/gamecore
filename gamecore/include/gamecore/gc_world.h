#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_abort.h"
#include "gamecore/gc_assert.h"

#include <unordered_map>
#include <typeinfo>
#include <memory>

#include <vec3.hpp>
#include <ext/quaternion_float.hpp>

/* The World contains all loaded entities in the game. */

namespace gc {

class World {
public:
    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    void update(float ts);

    Entity createEntity();

    size_t getComponentSignaturePosition(size_t hash);

    template <typename T>
    void registerComponent()
    {
        size_t hash = typeid(T).hash_code();
        GC_ASSERT(m_component_arrays.contains(hash) == false && "Registering component type more than once.");
        m_component_arrays.emplace(hash, std::make_unique<ComponentArray<T>>());

        size_t signature_position = m_next_signature_position;
        ++m_next_signature_position;
        GC_ASSERT(signature_position < MAX_COMPONENTS && "Registering too many components!");
        GC_ASSERT(m_component_signature_positions.contains(hash) == false);
        m_component_signature_positions.emplace(hash, signature_position);
    }

    template <typename T>
    T* getComponent(Entity entity)
    {
        // check if component exists on entity:
        size_t hash = typeid(T).hash_code();
        size_t signature_position = m_component_signature_positions.at(hash);
        const auto& entity_signature = m_signatures.at(entity);
        if (entity_signature.test(signature_position) == false) {
            return nullptr;
        }

        auto array = getComponentArray<T>();
        return array->getData(entity);
    }

    template <typename T>
    T* addComponent(Entity entity, T&& comp = T{})
    {
        size_t hash = typeid(T).hash_code();

        auto array = getComponentArray<T>();
        array->insertData(entity, std::move(comp)); // errors if entity already exists in array

        // set the component bit for this entity
        size_t signature_position = m_component_signature_positions.at(hash);
        auto& signature_ref = m_signatures.at(entity);
        signature_ref.set(signature_position);

        for (auto& [system_hash, system] : m_ecs_systems) {
            if (system->m_entities.contains(entity)) continue;
            if ((system->m_signature & signature_ref) == system->m_signature) {
                system->m_entities.insert(entity);
                system->onComponentInsert(entity);
            }
        }

        return array->getData(entity);
    }

    template <typename T>
    void registerSystem()
    {
        size_t hash = typeid(T).hash_code();
        m_ecs_systems.emplace_back(hash, std::make_unique<T>(this));
    }

    /* Pushes old systems starting at 'index' along by 1 */
    template <typename T>
    void registerSystemAtIndex(size_t index)
    {
        size_t hash = typeid(T).hash_code();
        m_ecs_systems.emplace(m_ecs_systems.begin() + index, hash, std::make_unique<T>(this));
    }

    template <typename T>
    T* getSystem()
    {
        size_t hash = typeid(T).hash_code();
        System* found_system = nullptr;
        for (auto& [system_hash, system] : m_ecs_systems) {
            if (hash == system_hash) found_system = system.get();
        }
        if (found_system == nullptr) {
            abortGame("Unable to find system");
        }
        T* casted_ptr = dynamic_cast<T*>(found_system);
        if (casted_ptr == nullptr) {
            abortGame("Failed to cast system pointer!");
        }
        return casted_ptr;
    }

public:
    Entity m_next_entity_id = 1; // 0 is not a valid entity
private:

    /* ecs stuff */

    size_t m_next_signature_position = 0;
    // maps component hashes to signature positions
    std::unordered_map<size_t, size_t> m_component_signature_positions{};
    // maps entity ids to their signatures. TODO: Make this a vector
    std::unordered_map<Entity, std::bitset<MAX_COMPONENTS>> m_signatures{};
    // maps component hashes to their arrays
    std::unordered_map<size_t, std::unique_ptr<IComponentArray>> m_component_arrays{};

    // hashes and associated systems
    std::vector<std::pair<size_t, std::unique_ptr<System>>> m_ecs_systems{};

    template <typename T>
    ComponentArray<T>* getComponentArray()
    {
        size_t hash = typeid(T).hash_code();
        auto it = m_component_arrays.find(hash);
        if (it == m_component_arrays.end()) {
            abortGame("Cannot find component array.");
        }
        auto ptr = it->second.get();
        auto casted_ptr = dynamic_cast<ComponentArray<T>*>(ptr);
        GC_ASSERT(casted_ptr != nullptr);
        return casted_ptr;
    }

};

} // namespace gc