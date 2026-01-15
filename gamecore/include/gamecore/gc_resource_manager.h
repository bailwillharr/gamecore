#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

#include "gamecore/gc_name.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"

namespace gc {

// register different types with the resource manager at runtime.
// Resources are immutable objects that are only stored by the resource manager and fetched with gc::Name handles.
// T::create() is defined for all resources.

class Content;         // forward-dec
class ResourceManager; // forward-dec

template <typename T>
concept ValidResource = requires(const Content& content_manager, Name name) {
    { T::create(content_manager, name) } -> std::same_as<T>;
};

extern std::atomic<uint32_t> g_next_resource_index;

// Produces a unique integer for a given type that can be used as an array index.
template <ValidResource T>
uint32_t getResourceIndex()
{
    static uint32_t index = g_next_resource_index.fetch_add(1, std::memory_order_relaxed);
    return index;
}

class IResourceCache {
public:
    virtual ~IResourceCache() = 0;
};

inline IResourceCache::~IResourceCache() = default;

template <ValidResource T>
class ResourceCache : public IResourceCache {
    std::unordered_map<Name, std::unique_ptr<T>> m_resources{};

public:
    const T& get(const Content& content_manager, Name name)
    {
        auto [it, just_created] = m_resources.emplace(name, std::make_unique<T>(T::create(content_manager, name)));
        return *(it->second);
    }
};

class ResourceManager {
    const Content& m_content_manager;

    std::vector<std::unique_ptr<IResourceCache>> m_caches{};

public:
    ResourceManager(const Content& content_manager) : m_content_manager(content_manager) { GC_TRACE("Initialised resource manager"); }
    ~ResourceManager() { GC_TRACE("Destroying resource manager..."); };

    template <ValidResource T>
    const T& get(Name name)
    {
        uint32_t index = getResourceIndex<T>();
        if (index >= m_caches.size()) {
            m_caches.emplace_back(std::make_unique<ResourceCache<T>>());
        }
        IResourceCache* i_cache = m_caches[index].get();
        ResourceCache<T>* cache = static_cast<ResourceCache<T>*>(i_cache);
        return cache->get(m_content_manager, name);
    }
};

} // namespace gc
