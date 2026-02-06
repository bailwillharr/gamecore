#pragma once

#include <vec2.hpp>
#include <vec3.hpp>
#include <vec4.hpp>

namespace gc {

struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 uv;

    static consteval int floatsPerVertex() { return static_cast<int>(sizeof(position) + sizeof(normal) + sizeof(tangent) + sizeof(uv)) / 4; }
};

} // namespace gc
