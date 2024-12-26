#pragma once

#include <filesystem>
#include <optional>

namespace gc {

/* path.empty() == true on failure */
std::optional<std::filesystem::path> findContentDir();

}