#pragma once

#include <string>
#include <vector>

namespace gc {

enum class ShaderModuleType : int { VERTEX, FRAGMENT };

// returns empty vector on failure
std::vector<uint32_t> compileShaderModule(const std::string& source, ShaderModuleType type);

} // namespace gc