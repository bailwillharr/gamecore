#pragma once

#include <queue>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <bit>
#include <bitset>
#include <type_traits>
#include <mutex>
#include <optional>
#include <format>

#include <asio/ip/udp.hpp>

#include "gamecore/gc_name.h"

namespace gc {

// All fields in packets are little endian
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
    void reset();

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
    void reset();

    size_t pos() const;
    size_t remaining() const;
};

constexpr size_t NET_MAX_PACKET_SIZE = 1200;

using NetPacketMagic = std::array<uint8_t, 5>;
using NetSessionToken = std::array<uint8_t, 16>;

constexpr NetPacketMagic NET_PACKET_MAGIC{'G', 'C', 'N', 'E', 'T'};
constexpr uint16_t NET_PACKET_VERSION = 1;

enum class NetPacketType : uint8_t {
    CONNECT_REQUEST = 0,
    CONNECT_CHALLENGE = 1,
    CONNECT_CHALLENGE_RESPONSE = 2,
    MESSAGE = 3,
};

struct NetPacketHeader {
    NetPacketMagic magic;
    uint16_t version;
    NetSessionToken token; // 0 if type==CONNECT_REQUEST, since there is no session
    NetPacketType type;

    void serialise(NetByteWriter& writer) const
    {
        writer.writeBytes(magic);
        writer.writeU16(version);
        writer.writeBytes(token);
        static_assert(std::is_same_v<std::underlying_type_t<NetPacketType>, uint8_t>);
        writer.writeU8(static_cast<uint8_t>(type));
    }

    static NetPacketHeader deserialise(NetByteReader& reader)
    {
        NetPacketHeader obj{};
        reader.readBytes(obj.magic);
        obj.version = reader.readU16();
        reader.readBytes(obj.token);
        static_assert(std::is_same_v<std::underlying_type_t<NetPacketType>, uint8_t>);
        obj.type = static_cast<NetPacketType>(reader.readU8());
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(magic) + sizeof(version) + sizeof(token) + sizeof(type); }

    static NetPacketHeader createValid(NetPacketType type, NetSessionToken token = {})
    {
        NetPacketHeader header{};
        header.magic = NET_PACKET_MAGIC;
        header.version = NET_PACKET_VERSION;
        header.token = std::move(token);
        header.type = type;
        return header;
    }
};

// client -> server
// token in header is 0
struct NetPacketConnectRequest {
    static constexpr NetPacketType TYPE = NetPacketType::CONNECT_REQUEST;
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

// server -> client
// token in header is set
struct NetPacketConnectChallenge {
    static constexpr NetPacketType TYPE = NetPacketType::CONNECT_CHALLENGE;

    uint64_t client_nonce; // unique per connection request

    void serialise(NetByteWriter& writer) const { writer.writeU64(client_nonce); }

    static NetPacketConnectChallenge deserialise(NetByteReader& reader)
    {
        NetPacketConnectChallenge obj{};
        obj.client_nonce = reader.readU64();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(client_nonce); }
};

// to prevent packet amplification
static_assert(NetPacketConnectRequest::getSerialisedSize() > NetPacketConnectChallenge::getSerialisedSize());

// client -> server
// token in header is set
struct NetPacketConnectChallengeResponse {
    static constexpr NetPacketType TYPE = NetPacketType::CONNECT_CHALLENGE_RESPONSE;

    uint64_t client_nonce; // unique per connection request

    void serialise(NetByteWriter& writer) const { writer.writeU64(client_nonce); }

    static NetPacketConnectChallengeResponse deserialise(NetByteReader& reader)
    {
        NetPacketConnectChallengeResponse obj{};
        obj.client_nonce = reader.readU64();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(client_nonce); }
};

// type = 0 and size = 0 is reserved by the net backend for empty keepalive packets.
// Other types are application defined.
struct NetPacketMessage {
    static constexpr NetPacketType TYPE = NetPacketType::MESSAGE;

    uint16_t seq_num;
    uint16_t ack_num;
    std::bitset<32> ack_bits;
    uint16_t payload_type;
    uint16_t payload_size;

    void serialise(NetByteWriter& writer) const
    {
        writer.writeU16(seq_num);
        writer.writeU16(ack_num);
        writer.writeU32(static_cast<uint32_t>(ack_bits.to_ulong()));
        writer.writeU16(payload_type);
        writer.writeU16(payload_size);
    }

    static NetPacketMessage deserialise(NetByteReader& reader)
    {
        NetPacketMessage obj{};
        obj.seq_num = reader.readU16();
        obj.ack_num = reader.readU16();
        obj.ack_bits = reader.readU32();
        obj.payload_type = reader.readU16();
        obj.payload_size = reader.readU16();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(seq_num) + sizeof(ack_num) + sizeof(ack_bits) + sizeof(payload_type) + sizeof(payload_size); }
};

template <typename T>
std::optional<T> tryDeserialise(NetByteReader& reader)
{
    if (reader.remaining() < T::getSerialisedSize()) {
        return std::nullopt;
    }
    return T::deserialise(reader);
}

template <typename T>
std::optional<T> tryDeserialiseExact(NetByteReader& reader)
{
    if (reader.remaining() != T::getSerialisedSize()) {
        return std::nullopt;
    }
    return T::deserialise(reader);
}

template <typename T>
void writePacketWithHeader(NetByteWriter& writer, NetSessionToken token, const T& payload)
{
    static_assert(!std::is_same_v<T, NetPacketHeader>);
    NetPacketHeader::createValid(T::TYPE, token).serialise(writer);
    payload.serialise(writer);
}

struct NetEvent {
    Name type;
};

class NetEventQueue {
    std::mutex m_mutex{};
    std::queue<NetEvent> m_queue{};

public:
    void push(NetEvent event);

    // returns true if an event was popped
    bool pop(NetEvent& ev);
};

bool verifyPacketHeader(const NetPacketHeader& header);

} // namespace gc

template <>
struct std::hash<gc::NetSessionToken> {
    size_t operator()(const gc::NetSessionToken& token) const noexcept
    {
        // truncating works as a hash function since NetSessionToken is already high entropy
        static_assert(sizeof(token) >= sizeof(size_t));
        size_t value;
        std::memcpy(&value, token.data(), sizeof(value));
        return value;
    }
};

template <>
struct std::formatter<asio::ip::udp::endpoint> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const asio::ip::udp::endpoint& endpoint, std::format_context& ctx) const
    {
        return std::format_to(ctx.out(), "{}:{}", endpoint.address().to_string(), endpoint.port());
    }
};
