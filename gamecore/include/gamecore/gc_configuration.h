#pragma once

/*
 * A class to store config values in memory with methods to [de]serialise config files
 */

/*
 * Plan:
 * Just make a light wrapper around the json library representation of key:value fields.
 * Keys could be described as strings, possibly with a "category.subcategory.key" syntax for json scopes.
 */

#include <nlohmann/json.hpp>

namespace gc {

struct Configuration {};

} // namespace gc
