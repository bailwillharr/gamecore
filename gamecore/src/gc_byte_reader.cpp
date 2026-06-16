#include "gamecore/gc_byte_reader.h"

#include <cstring>

#include "gamecore/gc_assert.h"

namespace gc {

static_assert(std::endian::native == std::endian::little, "only little-endian systems supported");
static_assert(std::numeric_limits<float>::is_iec559, "floats must be IEEE754");

ByteReader::ByteReader(std::span<const uint8_t> buffer) : m_buffer(buffer) {}

uint8_t ByteReader::readU8()
{
    uint8_t v{};
    GC_ASSERT(m_pos + sizeof(uint8_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint8_t));
    m_pos += sizeof(uint8_t);
    return v;
}

uint16_t ByteReader::readU16()
{
    uint16_t v{};
    GC_ASSERT(m_pos + sizeof(uint16_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint16_t));
    m_pos += sizeof(uint16_t);
    return v;
}

uint32_t ByteReader::readU32()
{
    uint32_t v{};
    GC_ASSERT(m_pos + sizeof(uint32_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint32_t));
    m_pos += sizeof(uint32_t);
    return v;
}

uint64_t ByteReader::readU64()
{
    uint64_t v{};
    GC_ASSERT(m_pos + sizeof(uint64_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint64_t));
    m_pos += sizeof(uint64_t);
    return v;
}

float ByteReader::readF32()
{
    float v{};
    GC_ASSERT(m_pos + sizeof(float) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(float));
    m_pos += sizeof(float);
    return v;
}

void ByteReader::readBytes(std::span<uint8_t> out)
{
    if (out.empty()) return;
    GC_ASSERT(m_pos + out.size() <= m_buffer.size());
    std::memcpy(out.data(), m_buffer.data() + m_pos, out.size());
    m_pos += out.size();
}

void ByteReader::skip(size_t n)
{
    GC_ASSERT(m_pos + n <= m_buffer.size());
    m_pos += n;
}

void ByteReader::reset() { m_pos = 0; }

size_t ByteReader::pos() const { return m_pos; }

size_t ByteReader::remaining() const { return m_buffer.size() - m_pos; }

} // namespace gc
