#pragma once

#include <deque>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/ip/udp.hpp>

#include "gamecore/gc_name.h"
#include "gamecore/gc_net_client.h"
#include "gamecore/gc_net_server.h"

namespace gc {

constexpr uint16_t NET_DEFAULT_SERVER_PORT{6969};

enum class NetMode { DISCONNECTED, SERVER, CLIENT };

class Net {
    std::variant<std::monostate, NetServer, NetClient> m_server_client{};
    std::deque<NetEvent> m_pending_events{}; // events left over from a server or client that no longer exists
    NetSimConfig m_sim_config{};
    NetDisconnectReason m_last_disconnect_reason{NetDisconnectReason::NONE};

public:
    // returns false on failure
    bool startServer(asio::ip::udp::endpoint endpoint);

    // only use in server mode
    void stopServer();
    void disconnectPeer(NetPeerId peer);

    // returns false on failure
    bool connectToServer(const asio::ip::udp::endpoint& endpoint);

    // only use in client mode
    void disconnectFromServer();
    NetClientConnectionStatus getClientConnectionStatus() const;

    // The remaining functions can be used in all modes:
    NetMode getMode() const;

    // The address the server is listening on (server mode) or the address of the server (client mode)
    asio::ip::udp::endpoint getServerEndpoint() const;

    // NET_PEER_SERVER in server mode, the ID assigned by the server in client mode once connected, otherwise NET_PEER_NONE
    NetPeerId getLocalPeerId() const;

    // Returns the number of remote hosts (1 in client mode once connected, number of clients in server mode)
    uint32_t getRemoteCount() const;

    // Returns the connected remote hosts with statistics about their connections.
    // In client mode this is just the server, in server mode it is every client.
    std::vector<NetPeerInfo> getPeers() const;

    // Why the last connection to a server ended. NONE if there hasn't been one.
    NetDisconnectReason getLastDisconnectReason() const;

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);

    // Only ev.type and ev.data are sent.
    // peer is ignored in client mode.
    // In server mode, don't set peer for broadcast to all clients
    void postEvent(const NetEvent& ev, NetDelivery delivery = NetDelivery::RELIABLE, NetPeerId peer = NET_PEER_NONE);

    // Simulates a bad network link for the current server/client and any that are created later. See NetSimConfig.
    void setSimConfig(const NetSimConfig& config);
    NetSimConfig getSimConfig() const;

    // executes synchronously
    std::optional<asio::ip::udp::endpoint> resolve(std::string_view host, std::string_view service);

private:
    NetServer* getServer();
    const NetServer* getServer() const;
    NetClient* getClient();
    const NetClient* getClient() const;

    // Destroys the current server or client, keeping any events that haven't been polled yet
    void reset();
};

} // namespace gc
