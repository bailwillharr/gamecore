#pragma once

#include <cstdint>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_net_connection.h"
#include "gamecore/gc_net_socket.h"

namespace gc {

// A NetServer can only be started once. Create a new one to start again.
class NetServer {
    struct Session {
        NetPeerId peer_id;
        NetSessionToken token;
        asio::ip::udp::endpoint endpoint;
        NetConnection connection;

        Session(NetPeerId id, NetSessionToken session_token, const asio::ip::udp::endpoint& client_endpoint, NetClock::time_point now);
    };

    // state that the main thread reads
    struct SharedState {
        mutable std::mutex mutex{};
        asio::ip::udp::endpoint local_endpoint{};
        std::vector<NetPeerInfo> peers{};
    };

    using SessionMap = std::unordered_map<NetSessionToken, Session>;

    static constexpr uint32_t MAX_CLIENTS = 64;

    SharedState m_shared{};
    std::atomic<bool> m_running{false};
    NetEventQueue m_event_queue{};

    asio::io_context m_context{};
    NetSocket m_socket{m_context};
    std::jthread m_server_thread{};
    bool m_started{false}; // only accessed by main thread

    // All these members are only accessed by the server thread
    SessionMap m_sessions{};
    std::unordered_map<NetPeerId, NetSessionToken> m_peer_tokens{};
    std::unordered_map<NetSessionToken, NetClock::time_point> m_closed_tokens{}; // tokens that can't be used to open a session again, with expiry time
    std::array<uint64_t, 2> m_secret{};
    NetPeerId m_next_peer_id{NET_PEER_SERVER + 1};
    bool m_flush_scheduled{false};
    NetClock::time_point m_last_publish_time{};
    std::vector<std::vector<uint8_t>> m_received_messages{}; // scratch buffer

public:
    NetServer() = default;
    NetServer(const NetServer&) = delete;

    ~NetServer();

    NetServer& operator=(const NetServer&) = delete;

    bool start(const asio::ip::udp::endpoint& endpoint);

    // Tells all clients that the server is shutting down
    void stop();

    bool isRunning() const;

    asio::ip::udp::endpoint getLocalEndpoint() const;

    uint32_t getPeerCount() const;
    std::vector<NetPeerInfo> getPeers() const;

    bool poll(NetEvent& ev);

    // don't specify a peer for broadcast
    void sendMessage(std::vector<uint8_t> message, NetDelivery delivery, NetPeerId peer = NET_PEER_NONE);

    void disconnectPeer(NetPeerId peer);

    void setSimConfig(const NetSimConfig& config);

private:
    asio::awaitable<void> tickLoop();

    void onPacket(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender);
    void onConnectRequest(ByteReader& reader, const asio::ip::udp::endpoint& sender);
    void onConnectChallengeResponse(ByteReader& reader, NetSessionToken token, const asio::ip::udp::endpoint& sender);
    void onData(ByteReader& reader, Session& session);

    Session* findSession(NetSessionToken token, const asio::ip::udp::endpoint& sender);
    void removeSession(SessionMap::iterator it, NetDisconnectReason reason, bool notify_client);
    void queueMessage(Session& session, NetDelivery delivery, std::span<const uint8_t> message, std::vector<NetSessionToken>& overflowed);
    void flushSession(Session& session, NetClock::time_point now);
    void scheduleFlush();
    void sendConnectAccept(const Session& session, uint64_t client_nonce);
    void sendDisconnect(NetSessionToken token, const asio::ip::udp::endpoint& endpoint, NetDisconnectReason reason, bool bypass_simulator);
    void tick();
    void publishPeers();
    void closeAllSessions(NetDisconnectReason reason);
};

} // namespace gc
