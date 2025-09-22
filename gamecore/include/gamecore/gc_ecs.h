#pragma once

#include <cstdint>

#include <bitset>
#include <vector>
#include <limits>
#include <unordered_map>
#include <atomic>
#include <stack>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"

namespace gc {

class World;      // forward-dec
class System;     // forward-dec
class FrameState; // forward-dec

using Entity = uint32_t;

/* This might seem limiting but it greatly simplifies component management and prevents components from making heap allocations. */
template <typename T>
concept ValidComponent = std::is_trivially_copyable_v<T>;

template <typename T>
concept ValidDerivedSystem = requires(World& world) { T(world); } && std::is_base_of_v<System, T> && !std::is_same_v<System, T>;

constexpr Entity ENTITY_NONE = std::numeric_limits<Entity>::max();
constexpr size_t MAX_COMPONENTS = 32;

extern std::atomic<uint32_t> g_next_component_index;
extern std::atomic<uint32_t> g_next_system_index;
extern std::atomic<uint32_t> g_next_frame_state_object_index;

// Produces a unique integer for a given type that can be used as an array index.
template <ValidComponent T>
uint32_t getComponentIndex()
{
    static uint32_t index = g_next_component_index.fetch_add(1, std::memory_order_relaxed);
    GC_ASSERT(index < MAX_COMPONENTS);
    return index;
}

template <ValidDerivedSystem T>
uint32_t getSystemIndex()
{
    static uint32_t index = g_next_system_index.fetch_add(1, std::memory_order_relaxed);
    return index;
}

template <typename T>
uint32_t getFrameStateObjectIndex()
{
    static uint32_t index = g_next_frame_state_object_index.fetch_add(1, std::memory_order_relaxed);
    return index;
}

class Signature {
    std::bitset<MAX_COMPONENTS> m_bits{};

public:
    void setWithIndex(const uint32_t component_index, bool value = true)
    {
        GC_ASSERT(component_index < MAX_COMPONENTS);
        m_bits.set(component_index, value);
    }

    template <typename T>
    void set(bool value = true)
    {
        setWithIndex(getComponentIndex<T>(), value);
    }

    bool hasComponentIndex(const uint32_t component_index) const
    {
        GC_ASSERT(component_index < MAX_COMPONENTS);
        return m_bits.test(component_index);
    }

    template <typename... Ts>
    bool hasTypes() const
    {
        return (... && m_bits.test(getComponentIndex<Ts>()));
    }

    uint32_t componentCount() const { return static_cast<uint32_t>(m_bits.count()); }

    template <typename... Ts>
    static Signature fromTypes()
    {
        Signature sig{};
        (sig.set<Ts>(), ...);
        return sig;
    }
};

class IComponentArray {
public:
    virtual ~IComponentArray() = default;

    virtual void addComponent(Entity entity) = 0;
    virtual void removeComponent(Entity entity) = 0;
};

/*
 * Dense ComponentArrays should be used when a majority of entities have the component.
 * Sparse ComponentArrays should be used otherwise, especially if the component is very large.
 * The methods in this class don't actually check if an entity should have a component.
 * This class is just a storage backend while the World actually manages components.
 */
enum class ComponentArrayType { SPARSE, DENSE };

template <ValidComponent T, ComponentArrayType ArrayType>
class ComponentArray : public IComponentArray {
    static_assert(std::is_trivially_copyable_v<T>, "Component must be trivially copyable");

    std::vector<T> m_component_array{};
    std::unordered_map<Entity, uint32_t> m_entity_component_indices{}; // only used if sparse
    std::stack<uint32_t> m_free_indices{};                             // only used if sparse

public:
    void addComponent(const Entity entity) override
    {
        GC_ASSERT(entity != ENTITY_NONE);

        uint32_t index{};
        if constexpr (ArrayType == ComponentArrayType::SPARSE) {
            GC_ASSERT(!m_entity_component_indices.contains(entity));
            if (m_free_indices.empty()) {
                m_entity_component_indices.insert({entity, static_cast<uint32_t>(m_component_array.size())});
                index = static_cast<uint32_t>(m_component_array.size());
                m_component_array.resize(index + 1);
            }
            else {
                // can reuse a slot in m_component_array
                index = m_free_indices.top();
                m_free_indices.pop();
                GC_ASSERT(index < m_component_array.size());
                m_entity_component_indices.insert({entity, index});
                m_component_array[index] = T{};
            }
        }
        else { // ComponentArrayType::DENSE
            index = entity;
            if (index >= m_component_array.size()) {
                m_component_array.resize(index + 1);
            }
            else {
                m_component_array[index] = T{};
            }
        }
    }

    void removeComponent(const Entity entity) override
    {
        GC_ASSERT(entity != ENTITY_NONE);

        if constexpr (ArrayType == ComponentArrayType::SPARSE) {
            if (auto it = m_entity_component_indices.find(entity); it != m_entity_component_indices.end()) {
                m_free_indices.push(it->second);
                m_entity_component_indices.erase(it);
            }
            else {
                GC_TRACE("ComponentArray::removeComponent() called on entity {} that wasn't in sparse ComponentArray {} (id: {})", entity, typeid(T).name(),
                         getComponentIndex<T>());
            }
        }
        else { // ComponentArrayType::DENSE
            // do nothing
        }
    }

    // These references can be invalidated if addComponent() is called after
    T& get(const Entity entity)
    {
        GC_ASSERT(entity != ENTITY_NONE);

        uint32_t index{};
        if constexpr (ArrayType == ComponentArrayType::SPARSE) {
            GC_ASSERT(m_entity_component_indices.contains(entity));
            index = m_entity_component_indices[entity];
        }
        else { // ComponentArrayType::DENSE
            index = entity;
        }
        GC_ASSERT(index < static_cast<uint32_t>(m_component_array.size()));
        return m_component_array[index];
    }
};

class System {

protected:
    World& m_world;

public:
    explicit System(World& world);
    System(const System&) = delete;

    virtual ~System() {}

    System& operator=(const System&) = delete;

    virtual void onUpdate(FrameState& frame_state) = 0;
};

} // namespace gc
