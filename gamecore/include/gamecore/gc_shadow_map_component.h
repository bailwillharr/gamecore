#pragma once

#include <glm/mat4x4.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// The shadows that a prefab's directional light casts, baked into a shadow map when the prefab was made (gcpak_editor does this
// when it compiles a glTF file). The entity's children aren't special: the shadow map covers whatever was in the prefab when it
// was baked, and anything else in the world can receive those shadows but can't cast its own.
//
// The shadow map is fixed to the entity: its matrix is relative to the entity's transform, so a prefab that is moved or rotated
// (by its parent) takes its shadows with it. It is only right for the light direction it was baked with.
// Only one shadow map is drawn per world, as there is only one directional light.
struct ShadowMapComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("ShadowMapComponent");

public:
    Name m_shadow_map{}; // a SHADOW_MAP_R16 asset
    // From the entity's local space to the shadow map: x and y are texture coordinates (0 to 1) and z is the depth from the light
    // (0 nearest to it, 1 farthest), as stored in the shadow map.
    glm::mat4 m_matrix{1.0f};
    // To stop surfaces shadowing themselves. Both depend on the size of the shadow map's texels, so the baker picks them.
    float m_normal_bias = 0.0f; // metres. How far along its normal a surface is moved before looking it up
    float m_depth_bias = 0.0f;  // in shadow map depth (0 to 1)

public:
    ShadowMapComponent& setShadowMap(Name shadow_map)
    {
        m_shadow_map = shadow_map;
        return *this;
    }

    ShadowMapComponent& setMatrix(const glm::mat4& matrix)
    {
        m_matrix = matrix;
        return *this;
    }

    ShadowMapComponent& setBias(float normal_bias, float depth_bias)
    {
        m_normal_bias = normal_bias;
        m_depth_bias = depth_bias;
        return *this;
    }

    void serialise(ByteWriter& writer) const
    {
        writer.writeU32(m_shadow_map.getHash());
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                writer.writeF32(m_matrix[column][row]);
            }
        }
        writer.writeF32(m_normal_bias);
        writer.writeF32(m_depth_bias);
    }

    void deserialise(ByteReader& reader)
    {
        m_shadow_map = Name(reader.readU32());
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                m_matrix[column][row] = reader.readF32();
            }
        }
        m_normal_bias = reader.readF32();
        m_depth_bias = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return sizeof(uint32_t) + 18 * sizeof(float); }
};

} // namespace gc
