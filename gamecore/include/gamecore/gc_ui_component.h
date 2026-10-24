#pragma once

#include <cstdint>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// Shows a UI document (see gc_ui.h) for as long as the entity has this component. The UISystem loads the document when it first sees
// the component, and closes it when the component (or the entity) goes. Changing 'document' loads the new one in its place.
class UIComponent {
public:
    static constexpr auto NAME = Name::createConstexpr("UIComponent");

    Name document{};    // the document's asset name: its file's name, such as "hud.rml"_name
    bool visible{true}; // a hidden document stays loaded (and keeps its state), it just isn't drawn

public:
    void serialise(ByteWriter& writer) const
    {
        writer.writeU32(document.getHash());
        writer.writeU8(visible ? 1 : 0);
    }

    void deserialise(ByteReader& reader)
    {
        document = Name(reader.readU32());
        visible = reader.readU8() != 0;
    }

    static constexpr size_t getSerialisedSize() { return sizeof(uint32_t) + sizeof(uint8_t); }
};

} // namespace gc
