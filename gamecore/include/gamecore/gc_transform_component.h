#pragma once

#include <istream>
#include <ostream>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

static_assert(std::endian::native == std::endian::little);

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
    Name name{"entity"};

public:
    glm::vec3 getPosition() const { return m_position; }

    glm::quat getRotation() const { return m_rotation; }

    glm::vec3 getScale() const { return m_scale; }

    glm::vec3 getWorldPosition() const { return m_world_matrix[3]; }

    glm::mat4 getWorldMatrix() const { return m_world_matrix; }

    Entity getParent() const { return m_parent; }

    TransformComponent& setPosition(const glm::vec3& position)
    {
        m_position = position;
        m_dirty = true;
        return *this;
    }

    TransformComponent& setPosition(float x, float y, float z) { return setPosition(glm::vec3(x, y, z)); }

    TransformComponent& setRotation(const glm::quat& rotation)
    {
        m_rotation = rotation;
        m_dirty = true;
        return *this;
    }

    TransformComponent& setRotation(float w, float x, float y, float z) { return setRotation(glm::quat(w, x, y, z)); }

    TransformComponent& setScale(const glm::vec3& scale)
    {
        m_scale = scale;
        m_dirty = true;
        return *this;
    }

    TransformComponent& setScale(float x, float y, float z) { return setScale(glm::vec3(x, y, z)); }

    TransformComponent& setScale(float scale) { return setScale(glm::vec3(scale, scale, scale)); }

    void serialize(std::ostream& s, uint32_t parent) const
    {
        s.write(reinterpret_cast<const char*>(&m_position.x), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_position.y), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_position.z), sizeof(float));

        s.write(reinterpret_cast<const char*>(&m_rotation.x), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_rotation.y), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_rotation.z), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_rotation.w), sizeof(float));

        s.write(reinterpret_cast<const char*>(&m_scale.x), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_scale.y), sizeof(float));
        s.write(reinterpret_cast<const char*>(&m_scale.z), sizeof(float));

        // Entity handles cannot be serialised
        s.write(reinterpret_cast<const char*>(&parent), sizeof(uint32_t));

        const uint32_t name_hash = name.getHash();
        s.write(reinterpret_cast<const char*>(&name_hash), sizeof(uint32_t));
    }

    // returned TransformComponent's m_parent is always ENTITY_NONE
    static TransformComponent deserialize(std::istream& s, uint32_t& parent_out)
    {
        TransformComponent t{};

        s.read(reinterpret_cast<char*>(&t.m_position.x), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_position.y), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_position.z), sizeof(float));

        s.read(reinterpret_cast<char*>(&t.m_rotation.x), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_rotation.y), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_rotation.z), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_rotation.w), sizeof(float));

        s.read(reinterpret_cast<char*>(&t.m_scale.x), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_scale.y), sizeof(float));
        s.read(reinterpret_cast<char*>(&t.m_scale.z), sizeof(float));

        s.read(reinterpret_cast<char*>(&parent_out), sizeof(uint32_t));

        uint32_t name_hash{};
        s.read(reinterpret_cast<char*>(&name_hash), sizeof(uint32_t));
        t.name = Name(name_hash);

        return t;
    }
};

} // namespace gc
