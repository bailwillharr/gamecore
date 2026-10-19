#pragma once

#include <glm/vec3.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// Haze in the air: things fade into it with distance. It is thickest at the entity's height and thins out above it, as the air does.
//
// What things fade to is the sky behind them, if the world has a sky (see SkyboxComponent): the light that the haze scatters
// towards the camera is about what the sky looks like in that direction. Otherwise they fade to m_color.
// Only one fog is drawn per world: the last one found. Things are drawn with no fog if there isn't one.
struct FogComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("FogComponent");

public:
    // How much of the light is lost per metre, at the entity's height. 1 / density is the distance at which about two thirds of
    // what is behind the fog has faded.
    float m_density = 1.0f / 10000.0f;
    // How quickly the fog thins out going up: it is 1 / e as thick this many metres above the entity. Zero: the same everywhere
    float m_height_falloff = 1.0f / 1000.0f;
    glm::vec3 m_color{1.0f, 1.0f, 1.0f}; // the luminance (cd/m^2) of fog that nothing can be seen through, if there is no sky
    bool m_use_sky = true;               // fade to the sky, if there is one

public:
    FogComponent& setDensity(float density)
    {
        m_density = density;
        return *this;
    }

    FogComponent& setHeightFalloff(float height_falloff)
    {
        m_height_falloff = height_falloff;
        return *this;
    }

    FogComponent& setColor(const glm::vec3& color)
    {
        m_color = color;
        return *this;
    }

    FogComponent& setUseSky(bool use_sky)
    {
        m_use_sky = use_sky;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeF32(m_density);
        writer.writeF32(m_height_falloff);
        writer.writeF32(m_color.x);
        writer.writeF32(m_color.y);
        writer.writeF32(m_color.z);
        writer.writeU8(m_use_sky ? 1 : 0);
    }

    void deserialise(ByteReader& reader)
    {
        m_density = reader.readF32();
        m_height_falloff = reader.readF32();
        m_color.x = reader.readF32();
        m_color.y = reader.readF32();
        m_color.z = reader.readF32();
        m_use_sky = reader.readU8() != 0;
    }

    static constexpr size_t getSerialisedSize() { return 5 * sizeof(float) + sizeof(uint8_t); }
};

} // namespace gc
