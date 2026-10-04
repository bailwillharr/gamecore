#pragma once

#include <bit>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

static_assert(std::endian::native == std::endian::little);

namespace gc {

class TransformComponent {
    friend class TransformSystem;

public:
    static constexpr auto NAME = Name::createConstexpr("TransformComponent");

private:
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

    // The parent isn't serialised, as Entity handles are only meaningful at runtime. Prefabs store the hierarchy themselves.
    void serialise(ByteWriter& writer) const
    {
        writer.writeF32(m_position.x);
        writer.writeF32(m_position.y);
        writer.writeF32(m_position.z);

        writer.writeF32(m_rotation.x);
        writer.writeF32(m_rotation.y);
        writer.writeF32(m_rotation.z);
        writer.writeF32(m_rotation.w);

        writer.writeF32(m_scale.x);
        writer.writeF32(m_scale.y);
        writer.writeF32(m_scale.z);

        writer.writeU32(name.getHash());
    }

    // The parent is left as it is
    void deserialise(ByteReader& reader)
    {
        m_position.x = reader.readF32();
        m_position.y = reader.readF32();
        m_position.z = reader.readF32();

        m_rotation.x = reader.readF32();
        m_rotation.y = reader.readF32();
        m_rotation.z = reader.readF32();
        m_rotation.w = reader.readF32();

        m_scale.x = reader.readF32();
        m_scale.y = reader.readF32();
        m_scale.z = reader.readF32();

        name = Name(reader.readU32());

        m_dirty = true;
    }

    static constexpr size_t getSerialisedSize() { return 10 * sizeof(float) + sizeof(uint32_t); }
};

} // namespace gc
