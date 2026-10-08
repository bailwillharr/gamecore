#pragma once

#include <string>
#include <string_view>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// The sky: what is seen behind everything else, and the light that it gives the world (image based lighting).
//
// A sky is three cube maps, which gcpak_editor makes from an HDR panorama (a .hdr file): the sky as it is seen, its irradiance
// (the light that a diffuse surface facing each way gets from all of it) and its reflections (the sky as surfaces of each
// roughness reflect it). With them, every surface is lit by the part of the sky that it faces and reflects the part that it
// mirrors, instead of by the flat colour of an ambient light.
//
// So this replaces ambient lights (LightComponent, LightType::AMBIENT): while the world is lit by a sky, they aren't drawn. They
// can be left in a world, for when it isn't: before the sky has been uploaded, or when its lighting is turned off (here, or
// in the debug UI's Render menu).
//
// The sun isn't part of the sky's light, though it is seen in it. It was taken out when the cube maps were made, as a world's
// sun is a directional light, with shadows. The sky is turned with its entity: its up is the entity's +Z. It can also follow the
// directional light (see setFollowSun()).
// Only one sky is drawn per world: the last one found.
struct SkyboxComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("SkyboxComponent");

public:
    // CUBE_MAP_E5B9G9R9 assets
    Name m_sky{};
    Name m_irradiance{};
    Name m_specular{};
    // How bright the sky is: the illuminance of a surface facing up, in lux, from the whole sky without its sun. The same as the
    // intensity of an ambient light. A clear sky at noon gives about 10,000 to 20,000. The cube maps only say how bright each
    // part of the sky is compared to the rest.
    float m_illuminance = 10000.0f;
    bool m_visible = true;    // whether the sky is drawn
    bool m_lighting = true;   // whether the sky lights the world
    bool m_follow_sun = true; // see setFollowSun()

public:
    // The name of the sky's own asset, which is the name of the .hdr file it was made from (e.g. "evening.hdr"). The other two
    // cube maps are named after it.
    SkyboxComponent& setSky(std::string_view sky_asset_name)
    {
        const std::string name(sky_asset_name);
        m_sky = Name(name);
        m_irradiance = Name(name + "/irradiance");
        m_specular = Name(name + "/specular");
        return *this;
    }

    SkyboxComponent& setIlluminance(float illuminance)
    {
        m_illuminance = illuminance;
        return *this;
    }

    SkyboxComponent& setVisible(bool visible)
    {
        m_visible = visible;
        return *this;
    }

    SkyboxComponent& setLighting(bool lighting)
    {
        m_lighting = lighting;
        return *this;
    }

    // If set, the sky is turned about the world's vertical (Z) so that the sun in it is in the same compass direction as the
    // world's directional light: the sun is then seen where the light, and the shadows, say it is. How high the sun is can't
    // be matched that way, as that would tilt the horizon. Does nothing if the sky has no sun or the world has no directional
    // light.
    SkyboxComponent& setFollowSun(bool follow_sun)
    {
        m_follow_sun = follow_sun;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeU32(m_sky.getHash());
        writer.writeU32(m_irradiance.getHash());
        writer.writeU32(m_specular.getHash());
        writer.writeF32(m_illuminance);
        writer.writeU8(static_cast<uint8_t>((m_visible ? 1 : 0) | (m_lighting ? 2 : 0) | (m_follow_sun ? 4 : 0)));
    }

    void deserialise(ByteReader& reader)
    {
        m_sky = Name(reader.readU32());
        m_irradiance = Name(reader.readU32());
        m_specular = Name(reader.readU32());
        m_illuminance = reader.readF32();
        const uint8_t flags = reader.readU8();
        m_visible = (flags & 1) != 0;
        m_lighting = (flags & 2) != 0;
        m_follow_sun = (flags & 4) != 0;
    }

    static constexpr size_t getSerialisedSize() { return 3 * sizeof(uint32_t) + sizeof(float) + sizeof(uint8_t); }
};

} // namespace gc
