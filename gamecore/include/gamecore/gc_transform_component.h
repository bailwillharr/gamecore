#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

#include <vec3.hpp>
#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>

namespace gc {

class TransformComponent {
    friend class TransformSystem;

    glm::vec3 m_position{0.0f, 0.0f, 0.0f};
    glm::quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_scale{1.0f};
    Entity m_parent{ENTITY_NONE}; // set with TransformSystem::setParent()
    glm::mat4 m_world_matrix{1.0f};
    bool m_dirty{true};

public:
    Name name{strToName("entity")};

public:
    glm::vec3 getPosition() const { return m_position; }

    glm::quat getRotation() const { return m_rotation; }

    glm::vec3 getScale() const { return m_scale; }

    glm::mat4 getWorldMatrix() const { return m_world_matrix; }

    void setPosition(const glm::vec3& position)
    {
        m_position = position;
        m_dirty = true;
    }

    void setRotation(const glm::quat& rotation)
    {
        m_rotation = rotation;
        m_dirty = true;
    }

    void setScale(const glm::vec3& scale)
    {
        m_scale = scale;
        m_dirty = true;
    }
};

} // namespace gc