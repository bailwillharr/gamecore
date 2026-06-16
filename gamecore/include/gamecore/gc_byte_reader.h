#pragma once

#include <cstdint>

#include <span>

namespace gc {

class ByteReader {
    std::span<const uint8_t> m_buffer;
    size_t m_pos = 0;

public:
    ByteReader(std::span<const uint8_t> buffer);

    uint8_t readU8();
    uint16_t readU16();
    uint32_t readU32();
    uint64_t readU64();
    float readF32();
    void readBytes(std::span<uint8_t> out);

    void skip(size_t n);
    void reset();

    size_t pos() const;
    size_t remaining() const;
};

} // namespace gc
