#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <bit>
#include <type_traits>

namespace gc {

// All fields in packets are little endian 😎
static_assert(std::endian::native == std::endian::little);

class NetByteWriter {
    std::span<uint8_t> m_buffer;
    size_t m_pos = 0;

public:
    NetByteWriter(std::span<uint8_t> buffer);

    void writeU8(uint8_t v);
    void writeU16(uint16_t v);
    void writeU32(uint32_t v);
    void writeU64(uint64_t v);
    void writeBytes(std::span<const uint8_t> bytes);

    void skip(size_t n);

    size_t pos() const;
    size_t remaining() const;
};

class NetByteReader {
    std::span<const uint8_t> m_buffer;
    size_t m_pos = 0;

public:
    NetByteReader(std::span<const uint8_t> buffer);

    uint8_t readU8();
    uint16_t readU16();
    uint32_t readU32();
    uint64_t readU64();
    void readBytes(std::span<uint8_t> out);

    void skip(size_t n);

    size_t pos() const;
    size_t remaining() const;
};

constexpr size_t NET_MAX_PACKET_SIZE = 1200;

using NetPacketMagic = std::array<uint8_t, 5>;
using NetServerToken = std::array<uint8_t, 32>;

constexpr NetPacketMagic NET_PACKET_MAGIC{'G', 'C', 'N', 'E', 'T'};
constexpr uint16_t NET_PACKET_VERSION = 1;

enum class NetPacketType : uint8_t { CONNECT_REQUEST = 0, CONNECT_CHALLENGE = 1, CONNECT_CHALLENGE_RESPONSE = 2, INVALID };

struct NetPacketHeader {
    NetPacketMagic magic;
    uint16_t version;
    NetPacketType type;

    void serialise(NetByteWriter& writer) const
    {
        writer.writeBytes(magic);
        writer.writeU16(version);
        static_assert(std::is_same_v<std::underlying_type_t<NetPacketType>, uint8_t>);
        writer.writeU8(static_cast<uint8_t>(type));
    }

    static NetPacketHeader deserialise(NetByteReader& reader)
    {
        NetPacketHeader obj{};
        reader.readBytes(obj.magic);
        obj.version = reader.readU16();
        static_assert(std::is_same_v<std::underlying_type_t<NetPacketType>, uint8_t>);
        obj.type = static_cast<NetPacketType>(reader.readU8());
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(magic) + sizeof(version) + sizeof(type); }

    static NetPacketHeader createValid(NetPacketType type)
    {
        NetPacketHeader header{};
        header.magic = NET_PACKET_MAGIC;
        header.version = NET_PACKET_VERSION;
        header.type = type;
        return header;
    }
};

// client -> server
struct NetPacketConnectRequest {
    static constexpr size_t PADDING_SIZE_BYTES = 56;

    uint64_t client_nonce; // unique per connection request

    void serialise(NetByteWriter& writer) const
    {
        writer.skip(PADDING_SIZE_BYTES);
        writer.writeU64(client_nonce);
    }

    static NetPacketConnectRequest deserialise(NetByteReader& reader)
    {
        NetPacketConnectRequest obj{};
        reader.skip(PADDING_SIZE_BYTES);
        obj.client_nonce = reader.readU64();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return PADDING_SIZE_BYTES + sizeof(client_nonce); }
};

// server -> client AND client -> server
struct NetPacketConnectChallenge {
    NetServerToken token;
    uint64_t client_nonce; // unique per connection request
    uint32_t time_bucket;

    void serialise(NetByteWriter& writer) const
    {
        writer.writeBytes(token);
        writer.writeU64(client_nonce);
        writer.writeU32(time_bucket);
    }

    static NetPacketConnectChallenge deserialise(NetByteReader& reader)
    {
        NetPacketConnectChallenge obj{};
        reader.readBytes(obj.token);
        obj.client_nonce = reader.readU64();
        obj.time_bucket = reader.readU32();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(token) + sizeof(client_nonce) + sizeof(time_bucket); }
};

// to prevent packet amplification
static_assert(NetPacketConnectRequest::getSerialisedSize() > NetPacketConnectChallenge::getSerialisedSize());

} // namespace gc
