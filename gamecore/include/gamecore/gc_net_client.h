#pragma once

#include <cstdint>

#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_net_connection.h"
#include "gamecore/gc_net_socket.h"

namespace gc {

enum class NetClientConnectionStatus { DISCONNECTED, CONNECTING, CONNECTED };

// A NetClient can only connect once. Create a new one to connect again.
class NetClient {
    enum class Phase {
        REQUEST,   // sending connect requests, waiting for a challenge
        RESPONSE,  // sending challenge responses, waiting to be accepted
        CONNECTED, // exchanging data
        CLOSED,
    };

    // state that the main thread reads
    struct SharedState {
        mutable std::mutex mutex{};
        NetConnectionStats stats{};
    };

    SharedState m_shared{};
    std::atomic<NetClientConnectionStatus> m_status{NetClientConnectionStatus::DISCONNECTED};
    std::atomic<NetPeerId> m_local_peer_id{NET_PEER_NONE};
    NetEventQueue m_event_queue{};
    asio::ip::udp::endpoint m_server_endpoint{}; // not modified once the client thread has started

    asio::io_context m_context{};
    NetSocket m_socket{m_context};
    std::jthread m_client_thread{};
    bool m_started{false}; // only accessed by main thread

    // All these members are only accessed by the client thread
    Phase m_phase{Phase::REQUEST};
    uint64_t m_client_nonce{};
    NetSessionToken m_session_token{0};
    NetClock::time_point m_connect_start_time{};
    NetClock::time_point m_last_handshake_send_time{};
    std::optional<NetConnection> m_connection{};
    bool m_flush_scheduled{false};
    std::vector<std::vector<uint8_t>> m_received_messages{}; // scratch buffer

public:
    NetClient() = default;
    NetClient(const NetClient&) = delete;

    ~NetClient();

    NetClient& operator=(const NetClient&) = delete;

    // Returns false on failure. Otherwise the connection is established in the background, see getConnectionStatus().
    bool connect(const asio::ip::udp::endpoint& endpoint);

    // Tells the server that the client is leaving
    void disconnect();

    bool poll(NetEvent& ev);
    NetClientConnectionStatus getConnectionStatus() const;
    asio::ip::udp::endpoint getServerEndpoint() const;

    // The ID the server assigned to this client. NET_PEER_NONE until connected.
    NetPeerId getLocalPeerId() const;

    NetConnectionStats getStats() const;

    // Messages sent before the connection is established are discarded
    void sendMessage(std::vector<uint8_t> message, NetDelivery delivery);

    void setSimConfig(const NetSimConfig& config);

private:
    asio::awaitable<void> tickLoop();

    void onPacket(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender);
    void sendConnectRequest();
    void sendConnectChallengeResponse();
    void sendDisconnect(NetDisconnectReason reason);
    void flush(NetClock::time_point now);
    void scheduleFlush();
    void tick();
    void close(NetDisconnectReason reason, bool notify_server);
};

} // namespace gc
