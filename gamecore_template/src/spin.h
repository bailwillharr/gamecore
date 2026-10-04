#pragma once

#include <glm/vec3.hpp>
#include <glm/geometric.hpp>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

class SpinComponent {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("SpinComponent");

private:
    friend class SpinSystem;
    float m_angle_radians{};
    glm::vec3 m_axis_norm{0.0f, 1.0f, 0.0f};
    float m_radians_per_second{1.0f};

public:
    SpinComponent& setRadiansPerSecond(float radians_per_second)
    {
        m_radians_per_second = radians_per_second;
        return *this;
    }
    SpinComponent& setAxis(const glm::vec3& axis)
    {
        m_axis_norm = glm::normalize(axis);
        return *this;
    }

    // Lets spinning entities be put in prefabs. The current angle is runtime state, so it isn't saved.
    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeF32(m_axis_norm.x);
        writer.writeF32(m_axis_norm.y);
        writer.writeF32(m_axis_norm.z);
        writer.writeF32(m_radians_per_second);
    }

    void deserialise(gc::ByteReader& reader)
    {
        m_axis_norm.x = reader.readF32();
        m_axis_norm.y = reader.readF32();
        m_axis_norm.z = reader.readF32();
        m_radians_per_second = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return 4 * sizeof(float); }
};

class SpinSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("SpinSystem");

public:
    SpinSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override;
};
