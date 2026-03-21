#pragma once

#include <thread>
#include <memory>
#include <queue>

#include <asio/io_context.hpp>

#include "gamecore/gc_name.h"

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

class Net {
    std::shared_ptr<asio::io_context> m_context{};
    std::shared_ptr<NetEventQueue> m_event_queue{};
    std::jthread m_server_thread{};

public:
    Net();

    void startServer();
    void stopServer();

    // returns true if an event is available
    bool pollEvents(NetEvent& ev);

    bool isServerRunning() const;
};

} // namespace gc
