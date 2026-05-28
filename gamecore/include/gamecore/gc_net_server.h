#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>
#include <queue>

#include "gamecore/gc_net_common.h"

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
};

struct NetSession {
    NetSessionToken session_token;
    asio::ip::udp::endpoint endpoint;
    uint64_t last_received_timestamp;
};

class NetServer {
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
    std::unordered_map<NetSessionToken, NetSession> m_sessions{};

public:
    ~NetServer();

    bool start(const asio::ip::udp::endpoint& endpoint);
    void stop();

    bool isRunning() const;
    asio::ip::udp::endpoint getLocalEndpoint() const;

private:
    // Can be called on the main thread
    void pushToOutboundQueue(OutboundPacket packet);

    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> keepAliveLoop();
};

} // namespace gc
