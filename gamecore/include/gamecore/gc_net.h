#pragma once

#include <variant>
#include <optional>

#include <asio/ip/udp.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>

#include "gamecore/gc_name.h"
#include "gamecore/gc_net_server.h"
#include "gamecore/gc_net_client.h"

namespace gc {

constexpr uint16_t NET_DEFAULT_SERVER_PORT{6969};

enum class NetMode { DISCONNECTED, SERVER, CLIENT };

class Net {
    std::variant<std::monostate, NetServer, NetClient> m_server_client;

public:
    // returns false on failure
    bool startServer(asio::ip::udp::endpoint endpoint);

    // only use in server mode
    void stopServer();
    asio::ip::udp::endpoint getServerEndpoint() const;

    // returns false on failure
    bool connectToServer(const asio::ip::udp::endpoint& endpoint);

    // only use in client mode
    void disconnectFromServer();
    NetClientConnectionStatus getClientConnectionStatus() const;

    // The remaining functions can be used in all modes:
    NetMode getMode() const;

    // Returns the number of remote hosts (1 in client mode, number of clients in server mode)
    uint32_t getRemoteCount() const;

    // returns empty vector if not in server mode
    std::vector<NetSessionToken> getRemoteSessions() const;

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);

    // session_token is ignored in client mode.
    // In server mode, don't set session_token for broadcast to all clients
    void postEvent(NetEvent ev, NetSessionToken session_token = 0);

    // executes synchronously
    std::optional<asio::ip::udp::endpoint> resolve(std::string_view host, std::string_view service);

private:
    NetServer& getServer();
    const NetServer& getServer() const;
    NetClient& getClient();
    const NetClient& getClient() const;
};

} // namespace gc
