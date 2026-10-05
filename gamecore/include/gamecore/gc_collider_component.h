#pragma once

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// Makes an entity solid: the CollisionSystem's queries (ray casts and so on) can hit the triangles of its mesh.
// The mesh is usually the one that the entity's RenderableComponent draws, but it can be any mesh, such as a simpler one.
// The component only says which mesh. Everything worked out from it (bounding boxes, the triangles) is kept by the
// CollisionSystem, which notices when the mesh here is changed, or when the entity moves.
struct ColliderComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("ColliderComponent");

public:
    Name m_mesh{}; // a mesh resource. Nothing can hit the entity while it is empty

public:
    ColliderComponent& setMesh(Name mesh)
    {
        m_mesh = mesh;
        return *this;
    }

    void serialise(ByteWriter& writer) const { writer.writeU32(m_mesh.getHash()); }

    void deserialise(ByteReader& reader) { m_mesh = Name(reader.readU32()); }

    static constexpr size_t getSerialisedSize() { return sizeof(uint32_t); }
};

} // namespace gc
