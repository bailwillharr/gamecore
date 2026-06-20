#include "gamecore/gc_byte_writer.h"

#include <cstring>

#include <bit>
#include <limits>

#include "gamecore/gc_assert.h"

namespace gc {

static_assert(std::endian::native == std::endian::little, "only little-endian systems supported");
static_assert(std::numeric_limits<float>::is_iec559, "floats must be IEEE754");

ByteWriter::ByteWriter(std::span<uint8_t> buffer) : m_buffer(buffer) {}

void ByteWriter::writeU8(uint8_t v)
{
    GC_ASSERT(m_pos + sizeof(uint8_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint8_t));
    m_pos += sizeof(uint8_t);
}

void ByteWriter::writeU16(uint16_t v)
{
    GC_ASSERT(m_pos + sizeof(uint16_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint16_t));
    m_pos += sizeof(uint16_t);
}

void ByteWriter::writeU32(uint32_t v)
{
    GC_ASSERT(m_pos + sizeof(uint32_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint32_t));
    m_pos += sizeof(uint32_t);
}

void ByteWriter::writeU64(uint64_t v)
{
    GC_ASSERT(m_pos + sizeof(uint64_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint64_t));
    m_pos += sizeof(uint64_t);
}

void ByteWriter::writeF32(float v)
{
    GC_ASSERT(m_pos + sizeof(float) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(float));
    m_pos += sizeof(float);
}

void ByteWriter::writeBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty()) return;
    GC_ASSERT(m_pos + bytes.size() <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, bytes.data(), bytes.size());
    m_pos += bytes.size();
}

void ByteWriter::skip(size_t n)
{
    GC_ASSERT(m_pos + n <= m_buffer.size());
    m_pos += n;
}

void ByteWriter::reset() { m_pos = 0; }

size_t ByteWriter::pos() const { return m_pos; }

size_t ByteWriter::remaining() const { return m_buffer.size() - m_pos; }

} // namespace gc
