#pragma once

#include <thread>

#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>

#include "gamecore/gc_net_common.h"

namespace gc {

enum class NetClientState { DISCONNECTED, CONNECTING, CONNECTED };

class NetClient {
    asio::io_context m_context{};
    std::jthread m_client_thread{};
    NetEventQueue m_event_queue{};
    std::atomic<NetClientState> m_state{};

public:
    ~NetClient();

    bool connect(const asio::ip::udp::endpoint& endpoint);
    void disconnect();
    bool poll(NetEvent& ev);
    NetClientState getState() const;

private:
    asio::awaitable<void> clientLoop(asio::ip::udp::endpoint server_endpoint);
};

} // namespace gc
