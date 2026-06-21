#pragma once

#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include <gctemplates/gct_static_vector.h>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_net_rto.h"

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
    bool running = false;
};

struct NetServerSession {
    NetSessionToken session_token{};
    asio::ip::udp::endpoint endpoint{};
    uint64_t last_receive_timestamp{0ULL};
    uint64_t last_send_timestamp{0ULL};
    uint16_t next_seq_num{0};    // post-incremented when sending
    uint16_t last_ack_num{UINT16_MAX};    // the highest received sequence number. init to 65535
    std::bitset<32> ack_bits{~0U}; // which of the last 32 client-side sequence numbers have been received. init to all 1s
    RetransmitTimeoutCalculator rto_calc{};

    struct QueuedPacket {
        static constexpr uint32_t MAX_ATTEMPTS = 4;
        uint64_t original_timestamp{};
        uint64_t last_send_timestamp{};
        uint32_t attempts{};
        std::shared_ptr<std::vector<uint8_t>> packet_data{};
    };
    std::unordered_map<uint16_t, QueuedPacket> retransmit_queue{}; // indexed by sequence number
};

struct NetServerActiveSessionsList {
    mutable std::mutex mut{};
    std::unordered_set<NetSessionToken> list{};
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
    struct OutboundRaw {
        NetSessionToken session_token;
        std::shared_ptr<std::vector<uint8_t>> packet_data;
    };
    using OutboundCommand = std::variant<OutboundConnectChallenge, OutboundUnicast, OutboundRaw>;
    using OutboundChannel = asio::experimental::channel<asio::io_context::executor_type, void(asio::error_code, OutboundCommand)>;

    static constexpr size_t OUTBOUND_QUEUE_MAX_SIZE = 1024;

    NetServerStatus m_server_status{};
    NetEventQueue m_event_queue{};

    std::jthread m_server_thread{};

    // Used to avoid having to lock m_sessions just to get the list of active sessions
    NetServerActiveSessionsList m_active_sessions_list{};

    // All these members are only accessed by the server thread
    asio::io_context m_context{};
    asio::ip::udp::socket m_socket{m_context};
    OutboundChannel m_outbound_queue{m_context.get_executor(), OUTBOUND_QUEUE_MAX_SIZE};
    std::unordered_map<NetSessionToken, NetServerSession> m_sessions{};

public:
    ~NetServer();

    bool start(const asio::ip::udp::endpoint& endpoint);
    void stop();

    bool isRunning() const;

    asio::ip::udp::endpoint getLocalEndpoint() const;

    uint32_t getActiveSessionsCount() const;
    std::vector<NetSessionToken> getActiveSessionsList() const;

    bool poll(NetEvent& ev);

    // don't specify a session token for broadcast
    void sendMessage(uint16_t payload_type, std::vector<uint8_t> payload, NetSessionToken token = 0);

private:
    // Can be called on the main thread
    void pushToOutboundQueue(OutboundCommand command);

    asio::awaitable<void> receiveLoop();
    asio::awaitable<void> sendLoop();
    asio::awaitable<void> keepAliveLoop();
};

} // namespace gc
