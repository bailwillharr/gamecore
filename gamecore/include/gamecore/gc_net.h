#pragma once

#include <thread>
#include <memory>
#include <queue>
#include <unordered_map>

#include <asio/ip/udp.hpp>
#include <asio/io_context.hpp>

#include "gamecore/gc_name.h"

namespace gc {

constexpr size_t NET_USERNAME_MAX_CHARS = 15;

struct NetSession {
    char username[NET_USERNAME_MAX_CHARS + 1];
};

struct NetServer {
    std::jthread server_thread{};
    std::mutex sessions_mutex{};
    std::unordered_map<asio::ip::udp::endpoint, NetSession> sessions{};
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
    asio::io_context m_context{};
    NetEventQueue m_event_queue{};
    NetServer m_server{};

public:
    Net();

    void startServer();
    void stop();

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);
};

} // namespace gc
