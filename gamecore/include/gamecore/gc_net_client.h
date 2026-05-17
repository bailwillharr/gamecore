#pragma once

#include <thread>
#include <optional>
#include <vector>

#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>

#include "gamecore/gc_net_common.h"

namespace gc {

enum class NetClientState { DISCONNECTED, CONNECTING, CONNECTED };

class NetClient {
    struct OutboundPacket {
        std::vector<uint8_t> data;
    };
    using OutboundChannel = asio::experimental::channel<asio::io_context::executor_type, void(asio::error_code, OutboundPacket)>;

    asio::io_context m_context{};
    std::jthread m_client_thread{};
    NetEventQueue m_event_queue{};
    std::atomic<NetClientState> m_state{};
    std::optional<asio::ip::udp::socket> m_socket{};
    OutboundChannel m_outbound_queue{m_context.get_executor(), 1024};
    std::optional<NetSessionToken> m_session_token{};
    std::optional<asio::steady_timer> m_recv_watchdog{};

public:
    ~NetClient();

    bool connect(const asio::ip::udp::endpoint& endpoint);
    void disconnect();
    bool poll(NetEvent& ev);
    NetClientState getState() const;

private:
    asio::awaitable<bool> handshake(asio::ip::udp::endpoint server_endpoint);
    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> heartbeatLoop();
    asio::awaitable<void> clientLoop(asio::ip::udp::endpoint server_endpoint);
};

} // namespace gc
