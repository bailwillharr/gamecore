#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

#include <vec3.hpp>
#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>

namespace gc {

struct TransformComponent {
    friend class TransformSystem;

    Name name;
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;

private:
    Entity parent = ENTITY_NONE; // set with TransformSystem::setParent()
    glm::mat4 world_matrix;
};

} // namespace gc