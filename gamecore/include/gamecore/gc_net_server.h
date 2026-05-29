#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>
#include <queue>
#include <variant>

#include <gctemplates/gct_static_vector.h>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_net_rto.h"

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
};

struct NetSession {
    NetSessionToken session_token;
    asio::ip::udp::endpoint endpoint;
    uint64_t last_receive_timestamp;
    uint64_t last_send_timestamp;
    uint16_t next_seq_num;    // post-incremented when sending
    uint16_t last_ack_num;    // the highest received sequence number
    std::bitset<32> ack_bits; // which of the last 32 client-side sequence numbers have been received
    RetransmitTimeoutCalculator rto_calc;

    struct QueuedPacket {
        static constexpr uint32_t MAX_ATTEMPTS = 4;
        static constexpr uint64_t MAX_AGE_NS = 500'000'000; // 500 ms
        uint64_t original_timestamp;
        uint64_t last_send_timestamp;
        uint32_t attempts;
        std::vector<uint8_t> packet_data;
    };
    std::unordered_map<uint16_t, QueuedPacket> retransmit_queue; // indexed by sequence number
};

class NetServer {
        struct OutboundConnectChallenge {
            asio::ip::udp::endpoint client_endpoint;
            NetSessionToken session_token; // no session exists yet
            uint64_t client_nonce;
        };
        struct OutboundUnicast {
            NetSessionToken session_token;
            uint16_t payload_type;
            std::vector<uint8_t> payload;
        };
        struct OutboundMulticast {
            uint16_t payload_type;
            std::vector<uint8_t> payload;
        };
        struct OutboundRaw {
            NetSessionToken session_token;
            std::vector<uint8_t> packet_data;
        };
        using OutboundCommand = std::variant<OutboundConnectChallenge, OutboundUnicast, OutboundMulticast, OutboundRaw>;

    using OutboundChannel = asio::experimental::channel<asio::io_context::executor_type, void(asio::error_code, OutboundCommand)>;

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

    // don't specify a session token for broadcast
    void sendMessage(std::optional<NetSessionToken> session_token, uint16_t payload_type, std::vector<uint8_t> payload);

private:
    // Can be called on the main thread
    void pushToOutboundQueue(OutboundCommand command);

    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> keepAliveLoop();
};

} // namespace gc
