#pragma once

#include <thread>
#include <memory>
#include <queue>
#include <unordered_map>

#include <asio/ip/udp.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>

#include "gamecore/gc_name.h"
#include "gamecore/gc_net_server.h"

namespace gc {

constexpr size_t NET_USERNAME_MAX_CHARS = 15;
constexpr std::array<uint8_t, 6> NET_PACKET_MAGIC{"GCNET"};
constexpr uint16_t NET_PACKET_VERSION = 1;

enum class NetPacketType : uint8_t {
    CONNECT_REQUEST = 0,
    CONNECT_CHALLENGE = 1,
    CONNECT_CHALLENGE_RESPONSE = 2,
    DISCONNECT = 3,
};

struct NetPacketHeader {
    std::array<uint8_t, 6> magic;
    uint8_t version;
    NetPacketType type;
};

// client -> server
struct NetPacketConnectRequest {
    uint64_t client_salt;
};

// server -> client
struct NetPacketConnectChallenge {
    uint64_t client_salt;
    uint64_t server_salt;
};

// client -> server
struct NetPacketChallengeResponse {
    uint64_t client_salt_xor_server_salt;
};

// client -> server
struct NetPacketDiscconect {
    uint64_t client_salt_xor_server_salt;
};

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

class Net {
    NetEventQueue m_event_queue{};
    NetServer m_server;

public:
    Net();

    void startServer();
    void stopServer();

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);
};

} // namespace gc
