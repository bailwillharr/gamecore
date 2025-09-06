#pragma once

#include <vector>
#include <span>
#include <mat4x4.hpp>

#include "gamecore/gc_ecs.h"

namespace gc {

class World; // forward-dec

class CubeSystem : public System {
    std::vector<glm::mat4> m_cube_transforms{};

public:
    CubeSystem(gc::World& world);

    void onUpdate(double dt) override;

    std::span<const glm::mat4> getCubeTransforms() const { return m_cube_transforms; }
};

} // namespace gc