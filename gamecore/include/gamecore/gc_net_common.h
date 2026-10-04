#pragma once

#include <cstddef>
#include <cstdint>

#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <type_traits>
#include <vector>

#include <asio/ip/udp.hpp>

#include "gamecore/gc_byte_reader.h"
#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_name.h"

namespace gc {

// All fields in packets are little endian
static_assert(std::endian::native == std::endian::little);

using NetClock = std::chrono::steady_clock;

using NetPacketMagic = std::array<uint8_t, 5>;

// Secret shared between a client and the server that authenticates every packet of a session.
// Never send a session token to a third party. Use NetPeerId to identify remote hosts instead.
using NetSessionToken = uint64_t;

// Identifies a host within a server's session. The server is always NET_PEER_SERVER, clients are assigned IDs by the server.
using NetPeerId = uint32_t;

constexpr size_t NET_MAX_PACKET_SIZE = 1200;

// The largest event payload that can be sent reliably. Messages that don't fit in a single packet are fragmented.
constexpr size_t NET_MAX_MESSAGE_SIZE = 256 * 1024;

constexpr NetPacketMagic NET_PACKET_MAGIC{'G', 'C', 'N', 'E', 'T'};
constexpr uint16_t NET_PACKET_VERSION = 2;

constexpr NetPeerId NET_PEER_NONE = 0; // when sending from a server, this means 'all clients'
constexpr NetPeerId NET_PEER_SERVER = 1;

enum class NetPacketType : uint8_t {
    CONNECT_REQUEST = 0,
    CONNECT_CHALLENGE = 1,
    CONNECT_CHALLENGE_RESPONSE = 2,
    CONNECT_ACCEPT = 3,
    DATA = 4,
    DISCONNECT = 5,
};

enum class NetDelivery : uint8_t {
    UNRELIABLE, // sent once. Can be lost, duplicated, or arrive out of order. Must fit in a single packet.
    RELIABLE,   // resent until acknowledged. Delivered exactly once and in the order they were sent.
};

// These values are sent over the wire in DISCONNECT packets
enum class NetDisconnectReason : uint8_t {
    NONE = 0,
    LOCAL_REQUEST = 1,       // this host closed the connection
    REMOTE_CLOSED = 2,       // the remote host closed the connection
    TIMED_OUT = 3,           // nothing was received from the remote host for too long
    KICKED = 4,              // the server removed this client
    SERVER_FULL = 5,         // the server has no free client slots
    SERVER_SHUTDOWN = 6,     // the server was stopped
    CONNECT_FAILED = 7,      // the server never answered the connection handshake
    SEND_QUEUE_OVERFLOW = 8, // reliable messages were queued faster than the remote host could acknowledge them
    SOCKET_FAILED = 9,       // the local socket failed
};

enum class NetEventKind : uint8_t {
    MESSAGE,      // 'type' and 'data' were sent by 'peer'
    CONNECTED,    // 'peer' is now connected. In client mode, 'peer' is always NET_PEER_SERVER
    DISCONNECTED, // 'peer' is no longer connected, see 'reason'
};

struct NetPacketHeader {
    NetPacketMagic magic;
    uint16_t version;
    NetSessionToken token; // 0 if type==CONNECT_REQUEST, since there is no session
    NetPacketType type;

    static_assert(sizeof(magic) == 5);
    static_assert(sizeof(version) == 2);
    static_assert(sizeof(token) == 8);
    static_assert(sizeof(type) == 1);

    void serialise(ByteWriter& writer) const
    {
        writer.writeBytes(magic);
        writer.writeU16(version);
        writer.writeU64(token);
        writer.writeU8(static_cast<uint8_t>(type));
    }

    static NetPacketHeader deserialise(ByteReader& reader)
    {
        NetPacketHeader obj{};
        reader.readBytes(obj.magic);
        obj.version = reader.readU16();
        obj.token = reader.readU64();
        static_assert(std::is_same_v<std::underlying_type_t<NetPacketType>, uint8_t>);
        obj.type = static_cast<NetPacketType>(reader.readU8());
        return obj;
    }

    static consteval size_t getSerialisedSize()
    {
        return 5    // magic
               + 2  // version
               + 8  // token
               + 1; // type
    }

    static NetPacketHeader createValid(NetPacketType type, NetSessionToken token = 0)
    {
        NetPacketHeader header{};
        header.magic = NET_PACKET_MAGIC;
        header.version = NET_PACKET_VERSION;
        header.token = token;
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

    void serialise(ByteWriter& writer) const
    {
        writer.skip(PADDING_SIZE_BYTES);
        writer.writeU64(client_nonce);
    }

    static NetPacketConnectRequest deserialise(ByteReader& reader)
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

    void serialise(ByteWriter& writer) const { writer.writeU64(client_nonce); }

    static NetPacketConnectChallenge deserialise(ByteReader& reader)
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

    void serialise(ByteWriter& writer) const { writer.writeU64(client_nonce); }

    static NetPacketConnectChallengeResponse deserialise(ByteReader& reader)
    {
        NetPacketConnectChallengeResponse obj{};
        obj.client_nonce = reader.readU64();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(client_nonce); }
};

// server -> client
// token in header is set
// Sent in reply to every valid challenge response. Tells the client that the session exists on the server.
struct NetPacketConnectAccept {
    static constexpr NetPacketType TYPE = NetPacketType::CONNECT_ACCEPT;

    uint64_t client_nonce; // unique per connection request
    NetPeerId peer_id;     // the ID the server assigned to the client

    void serialise(ByteWriter& writer) const
    {
        writer.writeU64(client_nonce);
        writer.writeU32(peer_id);
    }

    static NetPacketConnectAccept deserialise(ByteReader& reader)
    {
        NetPacketConnectAccept obj{};
        obj.client_nonce = reader.readU64();
        obj.peer_id = reader.readU32();
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(client_nonce) + sizeof(peer_id); }
};

// client <-> server
// token in header is set
// Not acknowledged, so it is sent more than once. If every copy is lost the remote host falls back to timing out.
struct NetPacketDisconnect {
    static constexpr NetPacketType TYPE = NetPacketType::DISCONNECT;

    NetDisconnectReason reason; // why the sender is closing the connection

    void serialise(ByteWriter& writer) const { writer.writeU8(static_cast<uint8_t>(reason)); }

    static NetPacketDisconnect deserialise(ByteReader& reader)
    {
        NetPacketDisconnect obj{};
        static_assert(std::is_same_v<std::underlying_type_t<NetDisconnectReason>, uint8_t>);
        obj.reason = static_cast<NetDisconnectReason>(reader.readU8());
        return obj;
    }

    static consteval size_t getSerialisedSize() { return sizeof(reason); }
};

// client <-> server
// token in header is set
// Followed by zero or more messages until the end of the packet, see gc_net_connection.cpp for their layout.
struct NetPacketData {
    static constexpr NetPacketType TYPE = NetPacketType::DATA;

    uint16_t seq_num;         // unique per packet, never reused for retransmissions
    uint16_t ack_num;         // the highest sequence number received from the remote host
    std::bitset<64> ack_bits; // bit N is set if sequence number (ack_num - N) was received
    uint16_t ack_delay;       // time between receiving ack_num and sending this packet, in units of 100 microseconds

    void serialise(ByteWriter& writer) const
    {
        writer.writeU16(seq_num);
        writer.writeU16(ack_num);
        writer.writeU64(ack_bits.to_ullong());
        writer.writeU16(ack_delay);
    }

    static NetPacketData deserialise(ByteReader& reader)
    {
        NetPacketData obj{};
        obj.seq_num = reader.readU16();
        obj.ack_num = reader.readU16();
        obj.ack_bits = reader.readU64();
        obj.ack_delay = reader.readU16();
        return obj;
    }

    static consteval size_t getSerialisedSize()
    {
        return 2    // seq_num
               + 2  // ack_num
               + 8  // ack_bits
               + 2; // ack_delay
    }
};

// Simulates a bad network link. Applied to both incoming and outgoing packets of the local socket.
struct NetSimConfig {
    bool enabled{false};
    float loss_percent{0.0f};      // chance of a packet being dropped
    float duplicate_percent{0.0f}; // chance of a packet being delivered twice
    float latency_ms{0.0f};        // one-way delay added to every packet
    float jitter_ms{0.0f};         // random variation (+/-) of the delay. Causes packets to arrive out of order
};

struct NetConnectionStats {
    float rtt_ms{};             // most recent round trip time sample
    float smoothed_rtt_ms{};    // round trip time averaged over recent samples
    float rto_ms{};             // current retransmission timeout
    float packet_loss{};        // fraction (0-1) of recently sent packets that were lost
    float send_rate{};          // bytes per second, excluding UDP/IP overhead
    float receive_rate{};       // bytes per second, excluding UDP/IP overhead
    float idle_ms{};            // time since the last packet was received
    float congestion_window{};  // how many packets of reliable data can be in flight at once
    uint32_t reliable_queue{};  // reliable fragments waiting to be acknowledged
    uint32_t unreliable_queue{}; // unreliable messages waiting to be sent
    uint64_t packets_sent{};
    uint64_t packets_received{};
    uint64_t packets_acked{};     // sent packets containing messages that were acknowledged
    uint64_t packets_lost{};      // sent packets containing messages that were never acknowledged
    uint64_t packets_duplicate{}; // received packets that were ignored because they were duplicates or too old
    uint64_t packets_malformed{};
    uint64_t bytes_sent{};
    uint64_t bytes_received{};
    uint64_t reliable_sent{}; // messages
    uint64_t reliable_received{};
    uint64_t reliable_resent{}; // fragments
    uint64_t unreliable_sent{};
    uint64_t unreliable_received{};
    uint64_t unreliable_dropped{}; // unreliable messages that were discarded before being sent
};

struct NetPeerInfo {
    NetPeerId id{};
    asio::ip::udp::endpoint endpoint{};
    NetConnectionStats stats{};
};

struct NetEvent {
    Name type{};
    std::vector<uint8_t> data{};
    NetEventKind kind{NetEventKind::MESSAGE};
    NetPeerId peer{NET_PEER_NONE};                    // the remote host this event came from
    NetDisconnectReason reason{NetDisconnectReason::NONE}; // only set if kind == DISCONNECTED
};

// thread-safe event queue
class NetEventQueue {
    std::mutex m_mutex{};
    std::queue<NetEvent> m_queue{};

public:
    void push(NetEvent event);

    // returns true if an event was popped
    bool pop(NetEvent& ev);
};

template <typename T>
std::optional<T> tryDeserialise(ByteReader& reader)
{
    if (reader.remaining() < T::getSerialisedSize()) {
        return std::nullopt;
    }
    return T::deserialise(reader);
}

template <typename T>
std::optional<T> tryDeserialiseExact(ByteReader& reader)
{
    if (reader.remaining() != T::getSerialisedSize()) {
        return std::nullopt;
    }
    return T::deserialise(reader);
}

template <typename T>
void writePacketWithHeader(ByteWriter& writer, NetSessionToken token, const T& payload)
{
    static_assert(!std::is_same_v<T, NetPacketHeader>);
    NetPacketHeader::createValid(T::TYPE, token).serialise(writer);
    payload.serialise(writer);
}

bool verifyPacketHeader(const NetPacketHeader& header);

int16_t seq_diff(uint16_t a, uint16_t b);

const char* netDisconnectReasonString(NetDisconnectReason reason);

// Events are sent as a message containing the hash of the event's type followed by its data.
std::vector<uint8_t> encodeNetEvent(const NetEvent& ev);

// Returns nullopt if the message is too short to be an event. 'message' is consumed.
std::optional<NetEvent> decodeNetEvent(std::vector<uint8_t>&& message, NetPeerId peer);

// Uses the operating system's entropy source
uint64_t generateNetRandom64();

} // namespace gc

template <>
struct std::formatter<asio::ip::udp::endpoint> {
    constexpr auto parse(std::format_parse_context& ctx) const { return ctx.begin(); }
    auto format(const asio::ip::udp::endpoint& endpoint, std::format_context& ctx) const
    {
        if (endpoint.address().is_v6()) {
            // brackets separate the address from the port
            return std::format_to(ctx.out(), "[{}]:{}", endpoint.address().to_string(), endpoint.port());
        }
        else {
            return std::format_to(ctx.out(), "{}:{}", endpoint.address().to_string(), endpoint.port());
        }
    }
};
