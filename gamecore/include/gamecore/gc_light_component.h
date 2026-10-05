#pragma once

#include <cstdint>

#include <glm/vec3.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

enum class LightType : uint8_t {
    POINT = 0,       // shines in every direction from the entity's position, as far as its range
    DIRECTIONAL = 1, // shines on the whole world along the entity's -Z axis (as cameras look), like the sun. Only one per world is drawn
    AMBIENT = 2,     // lights every surface in the world from every direction, like the sky. Nothing is lit by it unless there is one.
                     // Where the entity is doesn't matter. Several add up
};

struct LightComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("LightComponent");

public:
    LightType m_type = LightType::POINT;
    glm::vec3 m_color{1.0f, 1.0f, 1.0f}; // linear
    // In physical units, the same as glTF's. How bright it looks depends on the camera's exposure (see CameraComponent).
    // Directional: the illuminance of a surface facing the light, in lux (lm/m^2). The sun at noon is about 100,000.
    // Point: luminous intensity in candela (lm/sr). A surface d metres away gets intensity / d^2 lux. A 60 W bulb is about 70.
    // Ambient: the illuminance of a surface facing up, in lux. A surface facing down gets half of it. A clear sky is about 10,000.
    float m_intensity = 1000.0f;
    // Point lights only: the light fades to nothing at this distance. Zero means it has no limit (as real lights don't).
    float m_range = 0.0f;

public:
    LightComponent& setType(LightType type)
    {
        m_type = type;
        return *this;
    }

    LightComponent& setColor(const glm::vec3& color)
    {
        m_color = color;
        return *this;
    }

    LightComponent& setIntensity(float intensity)
    {
        m_intensity = intensity;
        return *this;
    }

    LightComponent& setRange(float range)
    {
        m_range = range;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeU8(static_cast<uint8_t>(m_type));
        writer.writeF32(m_color.r);
        writer.writeF32(m_color.g);
        writer.writeF32(m_color.b);
        writer.writeF32(m_intensity);
        writer.writeF32(m_range);
    }

    void deserialise(ByteReader& reader)
    {
        const uint8_t type = reader.readU8();
        m_type = (type <= static_cast<uint8_t>(LightType::AMBIENT)) ? static_cast<LightType>(type) : LightType::POINT;
        m_color.r = reader.readF32();
        m_color.g = reader.readF32();
        m_color.b = reader.readF32();
        m_intensity = reader.readF32();
        m_range = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return sizeof(uint8_t) + 5 * sizeof(float); }
};

} // namespace gc
