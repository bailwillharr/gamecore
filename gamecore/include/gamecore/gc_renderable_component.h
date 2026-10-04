#pragma once

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

struct RenderableComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("RenderableComponent");

public:
    bool m_visible = true;
    Name m_mesh{};     // can be empty
    Name m_material{}; // can be empty

public:
    RenderableComponent& setVisible(bool visible)
    {
        m_visible = visible;
        return *this;
    }

    RenderableComponent& setMesh(Name mesh)
    {
        m_mesh = mesh;
        return *this;
    }

    RenderableComponent& setMaterial(Name material)
    {
        m_material = material;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeU8(m_visible ? 1 : 0);
        writer.writeU32(m_mesh.getHash());
        writer.writeU32(m_material.getHash());
    }

    void deserialise(ByteReader& reader)
    {
        m_visible = reader.readU8() != 0;
        m_mesh = Name(reader.readU32());
        m_material = Name(reader.readU32());
    }

    static constexpr size_t getSerialisedSize() { return sizeof(uint8_t) + 2 * sizeof(uint32_t); }
};

} // namespace gc
