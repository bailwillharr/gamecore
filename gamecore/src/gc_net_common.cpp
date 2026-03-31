#include "gamecore/gc_net_common.h"

#include <cstring>

#include "gamecore/gc_assert.h"

namespace gc {

static_assert(std::endian::native == std::endian::little);

NetByteWriter::NetByteWriter(std::span<uint8_t> buffer) : m_buffer(buffer) {}

void NetByteWriter::writeU8(uint8_t v)
{
    GC_ASSERT(m_pos + sizeof(uint8_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint8_t));
    m_pos += sizeof(uint8_t);
}

void NetByteWriter::writeU16(uint16_t v)
{
    GC_ASSERT(m_pos + sizeof(uint16_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint16_t));
    m_pos += sizeof(uint16_t);
}

void NetByteWriter::writeU32(uint32_t v)
{
    GC_ASSERT(m_pos + sizeof(uint32_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint32_t));
    m_pos += sizeof(uint32_t);
}

void NetByteWriter::writeU64(uint64_t v)
{
    GC_ASSERT(m_pos + sizeof(uint64_t) <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, &v, sizeof(uint64_t));
    m_pos += sizeof(uint64_t);
}

void NetByteWriter::writeBytes(std::span<const uint8_t> bytes)
{
    GC_ASSERT(m_pos + bytes.size() <= m_buffer.size());
    std::memcpy(m_buffer.data() + m_pos, bytes.data(), bytes.size());
    m_pos += bytes.size();
}

void NetByteWriter::skip(size_t n)
{
    GC_ASSERT(m_pos + n < m_buffer.size());
    m_pos += n;
}

void NetByteWriter::reset() { m_pos = 0; }

size_t NetByteWriter::pos() const { return m_pos; }

size_t NetByteWriter::remaining() const { return m_buffer.size() - m_pos; }

NetByteReader::NetByteReader(std::span<const uint8_t> buffer) : m_buffer(buffer) {}

uint8_t NetByteReader::readU8()
{
    uint8_t v{};
    GC_ASSERT(m_pos + sizeof(uint8_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint8_t));
    m_pos += sizeof(uint8_t);
    return v;
}

uint16_t NetByteReader::readU16()
{
    uint16_t v{};
    GC_ASSERT(m_pos + sizeof(uint16_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint16_t));
    m_pos += sizeof(uint16_t);
    return v;
}

uint32_t NetByteReader::readU32()
{
    uint32_t v{};
    GC_ASSERT(m_pos + sizeof(uint32_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint32_t));
    m_pos += sizeof(uint32_t);
    return v;
}

uint64_t NetByteReader::readU64()
{
    uint64_t v{};
    GC_ASSERT(m_pos + sizeof(uint64_t) <= m_buffer.size());
    std::memcpy(&v, m_buffer.data() + m_pos, sizeof(uint64_t));
    m_pos += sizeof(uint64_t);
    return v;
}

void NetByteReader::readBytes(std::span<uint8_t> out)
{
    GC_ASSERT(m_pos + out.size() <= m_buffer.size());
    std::memcpy(out.data(), m_buffer.data() + m_pos, out.size());
    m_pos += out.size();
}

void NetByteReader::skip(size_t n)
{
    GC_ASSERT(m_pos + n < m_buffer.size());
    m_pos += n;
}

void NetByteReader::reset() { m_pos = 0; }

size_t NetByteReader::pos() const { return m_pos; }

size_t NetByteReader::remaining() const { return m_buffer.size() - m_pos; }

bool verifyPacketHeader(const NetPacketHeader& header)
{
    if (header.magic != NET_PACKET_MAGIC) {
        return false;
    }
    if (header.version != NET_PACKET_VERSION) {
        return false;
    }
    switch (header.type) {
    case NetPacketType::CONNECT_REQUEST:
        if (header.token != NetSessionToken{}) {
            return false;
        }
        break;
    case NetPacketType::CONNECT_CHALLENGE:
    case NetPacketType::CONNECT_CHALLENGE_RESPONSE:
    case NetPacketType::PING:
    case NetPacketType::PONG:
    case NetPacketType::GAME_RELIABLE_HEADER:
    case NetPacketType::GAME_UNRELIABLE_HEADER:
        if (header.token == NetSessionToken{}) {
            return false;
        }
        break;
    default:
        return false;
    }
    return true;
}

} // namespace gc
