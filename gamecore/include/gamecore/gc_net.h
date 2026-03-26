#pragma once

#include <queue>

#include <asio/ip/udp.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>

#include "gamecore/gc_name.h"
#include "gamecore/gc_net_server.h"

namespace gc {

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

enum class NetMode { DISCONNECTED, SERVER, CLIENT };

class Net {
    NetServer m_server;

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
};

} // namespace gc
