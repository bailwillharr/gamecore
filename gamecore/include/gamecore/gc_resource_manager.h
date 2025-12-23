#pragma once

#include "gamecore/gc_name.h"

namespace gc {

// register different types with the resource manager at runtime.
// Types would have a corresponding subclass which contains the instructions to build them
// For now, don't cache resources, just make them on demand

class ResourceManager;

template <typename T>
concept ValidResource = requires(gc::Name name) {
    { T::create(name) } -> std::same_as<T>;
};

class ResourceManager {

public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    template <ValidResource T>
    T get(gc::Name name)
    {
        return T::create(name);
    }
};

} // namespace gc
