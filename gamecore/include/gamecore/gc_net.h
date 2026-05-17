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
    size_t getConnectedClientCount() const;

    // executes synchronously
    std::optional<asio::ip::udp::endpoint> resolve(std::string_view host, std::string_view service);

private:
    NetServer& getServer();
    const NetServer& getServer() const;
    NetClient& getClient();
    const NetClient& getClient() const;
};

} // namespace gc
