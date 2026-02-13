#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

#include "gamecore/gc_name.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"

namespace gc {

// create() should have the following signature:
// static std::optional<ValidResource> create(const Content& content_manager, Name name);

// register different types with the resource manager at runtime.
// Resources are immutable objects that are only stored by the resource manager and fetched with gc::Name handles.
// However it is completely valid to copy a resource, modify the copy, and add the copy to the resource manager with a different name.
// T::create() is defined for all resources.

class Content;         // forward-dec
class ResourceManager; // forward-dec

template <typename T>
concept ValidResource = requires(const Content& content_manager, Name name) {
    { T::create(content_manager, name) } -> std::same_as<std::optional<T>>;
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
    const T* get(const Content& content_manager, Name name)
    {
        auto it = m_resources.find(name);
        if (it == m_resources.end()) {
            auto resource_opt = T::create(content_manager, name);
            if (resource_opt.has_value()) {
                it = m_resources.emplace(name, std::make_unique<T>(std::move(resource_opt.value()))).first;
            }
            else {
                return nullptr;
            }
        }
        return it->second.get();
    }

    // returns false if already exists
    bool add(T&& resource, Name name) { return m_resources.try_emplace(name, std::make_unique<T>(std::move(resource))).second; }

    void deleteResource(Name name) { m_resources.erase(name); }
};

class ResourceManager {
    const Content& m_content_manager;

    std::vector<std::unique_ptr<IResourceCache>> m_caches{};

public:
    ResourceManager(const Content& content_manager) : m_content_manager(content_manager) { GC_TRACE("Initialised resource manager"); }
    ~ResourceManager() { GC_TRACE("Destroying resource manager..."); };

    template <ValidResource T>
    const T* get(Name name)
    {
        if (name.empty()) {
            return nullptr;
        }
        uint32_t index = getResourceIndex<T>();
        if (index >= m_caches.size()) {
            m_caches.emplace_back(std::make_unique<ResourceCache<T>>());
            GC_ASSERT(index + 1 == m_caches.size());
        }
        IResourceCache* i_cache = m_caches[index].get();
        ResourceCache<T>* cache = static_cast<ResourceCache<T>*>(i_cache);
        return cache->get(m_content_manager, name);
    }

    // Generates random name if none given
    // Returns the name if successful otherwise empty on error (resource already exists)
    template <ValidResource T>
    Name add(T&& resource, Name name = {})
    {
        const uint32_t index = getResourceIndex<T>();
        if (index >= m_caches.size()) {
            m_caches.emplace_back(std::make_unique<ResourceCache<T>>());
            GC_ASSERT(index + 1 == m_caches.size());
        }
        IResourceCache* i_cache = m_caches[index].get();
        GC_ASSERT(i_cache);
        ResourceCache<T>* cache = static_cast<ResourceCache<T>*>(i_cache);
        if (name.empty()) {
            name = Name(static_cast<uint32_t>(rand() << 16 | rand()));
        }
        if (cache->add(std::move(resource), name)) {
            return name;
        }
        else {
            return {};
        }
    }

    // Deletes a resource from the cache.
    // This will invalidate references to that resource.
    template <ValidResource T>
    void deleteResource(Name name)
    {
        uint32_t index = getResourceIndex<T>();
        if (index < m_caches.size()) {
            IResourceCache* i_cache = m_caches[index].get();
            ResourceCache<T>* cache = static_cast<ResourceCache<T>*>(i_cache);
            cache->deleteResource(name);
        }
    }
};

} // namespace gc
