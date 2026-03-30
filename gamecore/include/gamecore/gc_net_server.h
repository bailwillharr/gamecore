#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>
#include <queue>

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
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

public:
    ~NetServer();

    bool start(const asio::ip::udp::endpoint& endpoint);
    void stop();

    bool isRunning() const;
    asio::ip::udp::endpoint getLocalEndpoint() const;

private:
    void pushToOutboundQueue(OutboundPacket packet);

    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
};

} // namespace gc
