#pragma once

#include <cstdint>

#include <atomic>
#include <thread>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "gamecore/gc_net_common.h"

namespace gc {

enum class NetClientConnectionStatus { DISCONNECTED, CONNECTING, CONNECTED };

class NetClient {
    struct OutboundMessage {
        uint16_t payload_type;
        std::vector<uint8_t> payload;
    };
    struct OutboundRaw {
        std::vector<uint8_t> packet_data;
    };
    using OutboundCommand = std::variant<OutboundMessage, OutboundRaw>;
    using OutboundChannel = asio::experimental::channel<asio::io_context::executor_type, void(asio::error_code, OutboundCommand)>;

    static constexpr size_t OUTBOUND_QUEUE_MAX_SIZE = 1024;

    asio::io_context m_context{};
    NetEventQueue m_event_queue{};
    OutboundChannel m_outbound_queue{m_context.get_executor(), OUTBOUND_QUEUE_MAX_SIZE};
    std::atomic<NetClientConnectionStatus> m_state{};

    std::jthread m_client_thread{};
    asio::ip::udp::socket m_socket{m_context};

public:
    ~NetClient();

    bool connect(const asio::ip::udp::endpoint& endpoint);
    void disconnect();
    bool poll(NetEvent& ev);
    NetClientConnectionStatus getConnectionStatus() const;

private:
};

} // namespace gc
