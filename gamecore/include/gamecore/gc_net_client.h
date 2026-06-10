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
#include "gamecore/gc_net_rto.h"

namespace gc {

enum class NetClientConnectionStatus { DISCONNECTED, CONNECTING, CONNECTED };

struct NetClientSession {
    NetSessionToken session_token{0};
    uint64_t last_receive_timestamp{0ULL};
    uint64_t last_send_timestamp{0ULL};
    uint16_t next_seq_num{0};          // post-incremented when sending
    uint16_t last_ack_num{UINT16_MAX}; // the highest received sequence number. init to 65535
    std::bitset<32> ack_bits{~0U};     // which of the last 32 server-side sequence numbers have been received. init to all 1s
    RetransmitTimeoutCalculator rto_calc{};

    struct QueuedPacket {
        static constexpr uint32_t MAX_ATTEMPTS = 4;
        uint64_t original_timestamp{};
        uint64_t last_send_timestamp{};
        uint32_t attempts{};
        std::vector<uint8_t> packet_data{};
    };
    std::unordered_map<uint16_t, QueuedPacket> retransmit_queue{}; // indexed by sequence number
};

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

    asio::ip::udp::endpoint m_server_endpoint{}; // only accessed by main thread

    asio::io_context m_context{};
    NetEventQueue m_event_queue{};
    OutboundChannel m_outbound_queue{m_context.get_executor(), OUTBOUND_QUEUE_MAX_SIZE};
    std::atomic<NetClientConnectionStatus> m_state{};

    std::jthread m_client_thread{};
    asio::ip::udp::socket m_socket{m_context}; // only accessed by client thread
    NetClientSession m_session{};              // only accessed by client thread

public:
    ~NetClient();

    bool connect(const asio::ip::udp::endpoint& endpoint);
    void disconnect();
    bool poll(NetEvent& ev);
    NetClientConnectionStatus getConnectionStatus() const;
    asio::ip::udp::endpoint getServerEndpoint() const;

    void sendMessage(uint16_t payload_type, std::vector<uint8_t> payload);

private:
    // Can be called on the main thread
    void pushToOutboundQueue(OutboundCommand command);

    asio::awaitable<void> sendLoop();
    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> keepAliveLoop();
};

} // namespace gc
