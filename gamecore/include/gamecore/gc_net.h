#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "gamecore/gc_name.h"

namespace gc {

using NetPeerId = uint64_t;
using NetTick = uint32_t;

constexpr NetPeerId NET_PEER_INVALID = 0;

// High-level runtime mode. Useful for deciding if local simulation is authoritative.
enum class NetMode : uint8_t {
    DISABLED = 0,
    CLIENT,
    DEDICATED_SERVER,
    LISTEN_SERVER,
};

// Ordered channels are intended for gameplay state or commands.
// Unordered channels are intended for telemetry/chat/debug payloads.
enum class NetChannel : uint8_t {
    RELIABLE_ORDERED = 0,
    RELIABLE_UNORDERED,
    UNRELIABLE_SEQUENCED,
};

struct NetAddress {
    std::string host{}; // IP/hostname
    uint16_t port{};
};

struct NetPacketView {
    NetChannel channel{NetChannel::RELIABLE_ORDERED};
    uint16_t message_type{};   // app-defined message id
    NetTick tick{};            // simulation tick associated with payload
    std::span<const uint8_t> payload{};
};

struct NetPacketOwned {
    NetChannel channel{NetChannel::RELIABLE_ORDERED};
    uint16_t message_type{};
    NetTick tick{};
    std::vector<uint8_t> payload{};
};

// Event stream produced by NetHost and consumed by main thread/systems.
struct NetEventConnected {
    NetPeerId peer_id{NET_PEER_INVALID};
    NetAddress remote{};
};

struct NetEventDisconnected {
    NetPeerId peer_id{NET_PEER_INVALID};
    std::string reason{};
};

struct NetEventPacket {
    NetPeerId peer_id{NET_PEER_INVALID};
    NetPacketOwned packet{};
};

struct NetEventWarning {
    std::string text{};
};

using NetEvent = std::variant<NetEventConnected, NetEventDisconnected, NetEventPacket, NetEventWarning>;

struct NetStats {
    uint32_t peers_connected{};
    uint64_t bytes_sent{};
    uint64_t bytes_received{};
    uint32_t inbound_queue_size{};
    uint32_t outbound_queue_size{};
};

struct NetHostConfig {
    NetMode mode{NetMode::DISABLED};

    // For CLIENT this is remote server address.
    // For LISTEN_SERVER/DEDICATED_SERVER this is local bind address.
    NetAddress endpoint{};

    // For listen server, simulation can emit/consume the same events as remote peers.
    bool enable_local_loopback_peer{false};

    // Optional label for debugging/profiling streams.
    Name host_name{};

    // Max events returned by pollEvents() to avoid starvation.
    uint32_t max_events_per_poll{1024};
};

// NetHost owns network I/O threads and transport state.
// Intentionally transport-agnostic at API level (UDP/TCP/Steam/etc can sit behind it).
class NetHost {
public:
    virtual ~NetHost() = default;

    virtual bool start(const NetHostConfig& config) = 0;
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;
    virtual NetMode mode() const = 0;

    // Called from main thread each frame/tick.
    // App can route returned events to hard-coded handlers and/or ECS systems.
    virtual std::vector<NetEvent> pollEvents() = 0;

    // Queue packets for async send from networking thread.
    virtual bool enqueueSend(NetPeerId peer_id, NetPacketOwned packet) = 0;
    virtual bool enqueueBroadcast(NetPacketOwned packet) = 0;

    // Optional utility for request/response style systems.
    virtual bool hasPeer(NetPeerId peer_id) const = 0;
    virtual NetStats stats() const = 0;

    // Connection management helpers.
    virtual bool connect(const NetAddress& remote) = 0; // valid in client mode
    virtual void disconnect(NetPeerId peer_id, std::string_view reason = {}) = 0;
};

// Factory declaration (implementation can live in gc_net.cpp later).
std::unique_ptr<NetHost> createNetHost();

} // namespace gc
