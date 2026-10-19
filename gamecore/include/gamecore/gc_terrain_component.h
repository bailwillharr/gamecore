#pragma once

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// Puts a terrain (a TERRAIN asset, see gc_terrain.h) in the world, where the entity's transform puts it. The terrain's space is
// the entity's: (0, 0) is a corner of the terrain, and z is the height, in metres, as the source data had it.
//
// The TerrainSystem draws it, streaming its tiles into GPU memory as the camera moves (see gc_terrain_system.h). The
// CollisionSystem makes it solid, if m_collision is set. The transform should only move and turn the terrain: the level of detail
// is chosen by distance, and the collision queries' distances are in the terrain's space, so scaling it would upset both.
struct TerrainComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("TerrainComponent");

public:
    Name m_terrain{};  // a TERRAIN asset
    Name m_material{}; // drawn with this material's textures, which are tiled across the ground. See content/shader_src/terrain.frag
    // How far from the camera the ground is drawn at full detail, in metres. Each level after that covers twice the distance of
    // the one before. More is sharper and slower. It is never less than about two tiles of the most detailed level.
    float m_detail_distance = 300.0f;
    bool m_visible = true;
    bool m_collision = true; // whether the CollisionSystem's queries hit it

public:
    TerrainComponent& setTerrain(Name terrain)
    {
        m_terrain = terrain;
        return *this;
    }

    TerrainComponent& setMaterial(Name material)
    {
        m_material = material;
        return *this;
    }

    TerrainComponent& setDetailDistance(float distance)
    {
        m_detail_distance = distance;
        return *this;
    }

    TerrainComponent& setVisible(bool visible)
    {
        m_visible = visible;
        return *this;
    }

    TerrainComponent& setCollision(bool collision)
    {
        m_collision = collision;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeU32(m_terrain.getHash());
        writer.writeU32(m_material.getHash());
        writer.writeF32(m_detail_distance);
        writer.writeU8(static_cast<uint8_t>((m_visible ? 1 : 0) | (m_collision ? 2 : 0)));
    }

    void deserialise(ByteReader& reader)
    {
        m_terrain = Name(reader.readU32());
        m_material = Name(reader.readU32());
        m_detail_distance = reader.readF32();
        const uint8_t flags = reader.readU8();
        m_visible = (flags & 1) != 0;
        m_collision = (flags & 2) != 0;
    }

    static constexpr size_t getSerialisedSize() { return 2 * sizeof(uint32_t) + sizeof(float) + sizeof(uint8_t); }
};

} // namespace gc
