#pragma once

#include <cstdint>

#include <span>

namespace gc {

class ByteWriter {
    std::span<uint8_t> m_buffer;
    size_t m_pos = 0;

public:
    ByteWriter(std::span<uint8_t> buffer);

    void writeU8(uint8_t v);
    void writeU16(uint16_t v);
    void writeU32(uint32_t v);
    void writeU64(uint64_t v);
    void writeF32(float v);
    void writeBytes(std::span<const uint8_t> bytes);

    void skip(size_t n);
    void reset();

    size_t pos() const;
    size_t remaining() const;
};

} // namespace gc
