#pragma once

#include <thread>
#include <optional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/steady_timer.hpp>

#include "gamecore/gc_net_common.h"

namespace gc {

enum class NetClientState { DISCONNECTED, CONNECTING, CONNECTED };

struct NetClientDebugInfo {
    NetClientState state{NetClientState::DISCONNECTED};
    double last_rtt_ms{};
    double avg_rtt_ms{};
    uint64_t ping_sent{};
    uint64_t pong_received{};
    uint64_t packets_sent{};
    uint64_t packets_received{};
};

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
    mutable std::mutex m_debug_mutex{};
    NetClientDebugInfo m_debug_info{};
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> m_ping_send_times{};

public:
    ~NetClient();

    bool connect(const asio::ip::udp::endpoint& endpoint);
    void disconnect();
    bool poll(NetEvent& ev);
    NetClientState getState() const;
    NetClientDebugInfo getDebugInfo() const;

private:
    asio::awaitable<bool> handshake(asio::ip::udp::endpoint server_endpoint);
    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> heartbeatLoop();
    asio::awaitable<void> clientLoop(asio::ip::udp::endpoint server_endpoint);
};

} // namespace gc
