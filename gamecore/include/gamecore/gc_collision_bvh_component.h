#pragma once

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// The collision trees of a prefab's meshes, built when the prefab was made instead of when it is loaded (gcpak_editor does this
// when it compiles a glTF file, and for any other prefab with --bake-collision). See gc_collision_bvh.h
//
// While an entity with this component is in the world, the CollisionSystem takes the tree of a collider's mesh from the asset,
// if it is in there, rather than building it. Nothing else changes: colliders are still ColliderComponents, can be anywhere and
// can move, and a mesh that isn't in the asset has its tree built as before. The entity itself doesn't matter (its transform
// isn't used), as the trees are in the space of their meshes.
//
// Any number of these can be in a world at once: one for each prefab that was baked. If a mesh is in several of the assets, one
// of them is used. They are all the same, as long as they were baked from the same mesh.
// A game has to register this component for a prefab's baked trees to be used. Otherwise the trees are built.
struct CollisionBvhComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("CollisionBvhComponent");

public:
    Name m_bvh{}; // a COLLISION_BVH asset

public:
    CollisionBvhComponent& setBvh(Name bvh)
    {
        m_bvh = bvh;
        return *this;
    }

    void serialise(ByteWriter& writer) const { writer.writeU32(m_bvh.getHash()); }

    void deserialise(ByteReader& reader) { m_bvh = Name(reader.readU32()); }

    static constexpr size_t getSerialisedSize() { return sizeof(uint32_t); }
};

} // namespace gc
