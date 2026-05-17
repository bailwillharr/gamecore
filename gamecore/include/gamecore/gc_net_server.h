#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>
#include <queue>
#include <chrono>

#include "gamecore/gc_net_common.h"

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
    size_t connected_client_count{};
    double avg_client_rtt_ms{};
    double worst_client_rtt_ms{};
    uint64_t ping_sent{};
    uint64_t pong_received{};
    uint64_t packets_sent{};
    uint64_t packets_received{};
};

struct NetServerDebugInfo {
    asio::ip::udp::endpoint local_endpoint{};
    size_t connected_client_count{};
    double avg_client_rtt_ms{};
    double worst_client_rtt_ms{};
    uint64_t ping_sent{};
    uint64_t pong_received{};
    uint64_t packets_sent{};
    uint64_t packets_received{};
};

class NetServer {
public:
    struct Session {
        NetSessionToken session_token;
        asio::ip::udp::endpoint endpoint;
        uint32_t time_bucket_last_received{};
        uint16_t next_ping_seq_num{};
        std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> ping_send_times{};
        double avg_rtt_ms{};
        double last_rtt_ms{};
    };

private:
    struct OutboundPacket {
        asio::ip::udp::endpoint endpoint;
        std::vector<uint8_t> data; // entire raw packet data
    };

    using OutboundChannel = asio::experimental::channel<asio::io_context::executor_type, void(asio::error_code, OutboundPacket)>;

    static constexpr size_t OUTBOUND_QUEUE_MAX_SIZE = 1024;

    NetServerStatus m_server_status{};

    std::jthread m_server_thread{};

    // All these members are only accessed by the server thread
    asio::io_context m_context{};
    std::optional<asio::ip::udp::socket> m_socket;
    OutboundChannel m_outbound_queue{m_context.get_executor(), OUTBOUND_QUEUE_MAX_SIZE};
    std::unordered_map<NetSessionToken, Session> m_sessions{};
    uint64_t m_server_secret{};

public:
    ~NetServer();

    bool start(const asio::ip::udp::endpoint& endpoint);
    void stop();

    bool isRunning() const;
    asio::ip::udp::endpoint getLocalEndpoint() const;
    size_t getConnectedClientCount() const;
    NetServerDebugInfo getDebugInfo() const;

private:
    // Can be called on the main thread
    void pushToOutboundQueue(OutboundPacket packet);

    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> heartbeatLoop();
    void updateDebugStatsFromSessions();
};

} // namespace gc
