#pragma once

#include <variant>

#include <asio/ip/udp.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>

#include "gamecore/gc_name.h"
#include "gamecore/gc_net_server.h"
#include "gamecore/gc_net_client.h"

namespace gc {

enum class NetMode { DISCONNECTED, SERVER, CLIENT };

class Net {
    std::variant<NetServer, NetClient> m_server_client;

    NetMode m_local_mode{NetMode::DISCONNECTED};

public:
    // returns false on failure
    bool startServer(asio::ip::udp::endpoint endpoint);

    void stopServer();

    // returns false on failure
    bool connectToServer(const asio::ip::udp::endpoint& endpoint);

    void disconnectFromServer();

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);

    NetMode getMode() const;

    NetClientState getClientState() const;

    asio::ip::udp::endpoint getServerEndpoint() const;
    bool isServerRunning() const;

private:
    NetServer& getServer();
    const NetServer& getServer() const;
    NetClient& getClient();
    const NetClient& getClient() const;
};

} // namespace gc
