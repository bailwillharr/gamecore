#pragma once

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

class LightComponent {
public:
    static constexpr auto NAME = Name::createConstexpr("LightComponent");

    // Lights have no properties yet, so there is nothing to serialise. Having these lets lights be put in prefabs.
    void serialise(ByteWriter&) const {}

    void deserialise(ByteReader&) {}

    static constexpr size_t getSerialisedSize() { return 0; }
};

} // namespace gc
