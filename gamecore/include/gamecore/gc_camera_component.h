#pragma once

#include <glm/trigonometric.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

class CameraSystem; // forward-dec

class CameraComponent {
    friend class CameraSystem;

public:
    static constexpr auto NAME = Name::createConstexpr("CameraComponent");

private:
    float m_fov_radians{glm::radians(45.0f)};
    float m_near = 0.1f;
    bool m_active = true;
    float m_exposure_ev100 = 15.0f;

public:
    CameraComponent& setFOV(float fov_radians)
    {
        m_fov_radians = fov_radians;
        return *this;
    }
    CameraComponent& setNearPlane(float near_plane)
    {
        m_near = near_plane;
        return *this;
    }
    CameraComponent& setActive(bool active)
    {
        m_active = active;
        return *this;
    }
    // Lights are in physical units, so the camera decides how bright the picture is, as a real camera does. This is the exposure
    // value at ISO 100: one more halves the brightness. About 15 suits full sunlight (100,000 lux), 7 a lit room (400 lux).
    // A white surface is well exposed when the EV is log2(illuminance / 3).
    CameraComponent& setExposure(float exposure_ev100)
    {
        m_exposure_ev100 = exposure_ev100;
        return *this;
    }

    float getExposure() const { return m_exposure_ev100; }
    float getFOV() const { return m_fov_radians; }
    float getNearPlane() const { return m_near; }
    bool getActive() const { return m_active; }

    void serialise(ByteWriter& writer) const
    {
        writer.writeF32(m_fov_radians);
        writer.writeF32(m_near);
        writer.writeU8(m_active ? 1 : 0);
        writer.writeF32(m_exposure_ev100);
    }

    void deserialise(ByteReader& reader)
    {
        m_fov_radians = reader.readF32();
        m_near = reader.readF32();
        m_active = reader.readU8() != 0;
        m_exposure_ev100 = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return 3 * sizeof(float) + sizeof(uint8_t); }
};

} // namespace gc
