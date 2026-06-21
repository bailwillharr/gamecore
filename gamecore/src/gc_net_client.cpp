#include "gamecore/gc_net_client.h"

#include <atomic>
#include <random>
#include <chrono>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <SDL3/SDL_timer.h>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_abort.h"

namespace gc {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

static uint32_t generateClientNonce()
{
    // TODO; use cryptographically secure RNG
    std::mt19937 rand32(std::random_device{}());
    return rand32();
}

static asio::awaitable<bool> checkSend(asio::ip::udp::socket& socket, std::span<const uint8_t> buffer)
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::error_code ec{};
    size_t bytes_transferred{};
    std::tie(ec, bytes_transferred) = co_await socket.async_send(asio::buffer(buffer), 0, TOKEN);
    if (ec || bytes_transferred < buffer.size()) {
        GC_ERROR("Failed to send: {}, sent {}/{}", ec.message(), bytes_transferred, buffer.size());
        co_return false;
    }
    else {
        co_return true;
    }
}

static asio::awaitable<size_t> checkReceive(asio::ip::udp::socket& socket, std::span<uint8_t> buffer)
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::error_code ec{};
    size_t bytes_transferred{};
    std::tie(ec, bytes_transferred) = co_await socket.async_receive(asio::buffer(buffer), 0, TOKEN);
    if (ec) {
        GC_ERROR("Failed to receive: {}", ec.message());
        co_return 0;
    }
    else {
        co_return bytes_transferred;
    }
}

static asio::awaitable<void> initiateConnection(asio::ip::udp::socket& socket, NetClientSession& out_session)
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    constexpr int NUM_CONNECT_ATTEMPTS = 6;
    constexpr auto CONNECT_ATTEMPT_COOLDOWN = std::chrono::milliseconds(40);

    const auto executor = co_await asio::this_coro::executor;

    asio::error_code ec{};

    std::vector<uint8_t> buf(NET_MAX_PACKET_SIZE);

    const auto client_nonce = generateClientNonce();

    uint64_t last_receive_timestamp{};
    uint64_t last_send_timestamp{};

    NetSessionToken session_token = 0;
    for (int i = 0; i < NUM_CONNECT_ATTEMPTS; ++i) {
        NetPacketConnectRequest connect_request{};
        connect_request.client_nonce = client_nonce;
        ByteWriter writer(buf);
        writePacketWithHeader(writer, NetSessionToken{0}, connect_request);
        last_send_timestamp = SDL_GetTicksNS();
        bool send_success = co_await checkSend(socket, std::span(buf.begin(), writer.pos()));
        if (!send_success) {
            co_return;
        }

        const size_t bytes_received = co_await checkReceive(socket, buf);
        ByteReader reader(std::span(buf.begin(), bytes_received));
        const auto received_header = tryDeserialise<NetPacketHeader>(reader);
        if (received_header && verifyPacketHeader(*received_header) && received_header->type == NetPacketType::CONNECT_CHALLENGE) {
            const auto received_challenge = tryDeserialiseExact<NetPacketConnectChallenge>(reader);
            if (received_challenge && received_challenge->client_nonce == client_nonce) {
                session_token = received_header->token;
                last_receive_timestamp = SDL_GetTicksNS();
                break;
            }
        }

        asio::steady_timer timer(executor, CONNECT_ATTEMPT_COOLDOWN);
        std::tie(ec) = co_await timer.async_wait(TOKEN);
        if (ec) {
            GC_ERROR("Timer error: {}", ec.message());
            co_return;
        }
    }
    if (session_token == 0) {
        co_return;
    }

    {
        NetPacketConnectChallengeResponse challenge_response{};
        challenge_response.client_nonce = client_nonce;
        ByteWriter writer(buf);
        writePacketWithHeader(writer, session_token, challenge_response);
        for (int i = 0; i < NUM_CONNECT_ATTEMPTS; ++i) {
            bool send_success = co_await checkSend(socket, std::span(buf.begin(), writer.pos()));
            if (!send_success) {
                co_return;
            }
        }
    }

    out_session.session_token = session_token;
    GC_ASSERT(last_receive_timestamp != 0);
    out_session.last_receive_timestamp = last_receive_timestamp;
    GC_ASSERT(last_send_timestamp != 0);
    out_session.last_send_timestamp = last_send_timestamp;
}

[[nodiscard]] static std::optional<NetEvent> handleMessage(ByteReader& reader, NetClientSession& session)
{
    const auto message = tryDeserialise<NetPacketMessage>(reader);
    if (!message) {
        return std::nullopt;
    }

    // read seq information
    const int16_t diff = seq_diff(message->seq_num, session.last_ack_num);
    bool read_message = false;
    if (diff > 0) {
        // new sequence number. slide window forward.
        session.ack_bits <<= diff;
        session.ack_bits.set(0, true);
        session.last_ack_num = message->seq_num;
        read_message = true;
    }
    else if (diff > -static_cast<int16_t>(session.ack_bits.size())) {
        GC_ASSERT(diff <= 0);
        if (session.ack_bits.test(-diff)) {
            GC_DEBUG("received packet {} already acknowledged", message->seq_num);
        }
        else {
            // previously missing sequence number, acknowledge it.
            GC_DEBUG("Received previously missing server message: {}", message->seq_num);
            session.ack_bits.set(-diff, true);
            read_message = true;
        }
    }
    // outdated packets (old sequence numbers) are ignored
    if (!read_message) {
        // since retransmissions are just copies, there's no need to look at the message's ack info
        return std::nullopt;
    }

    // Remove newly acked outbound packets from queue...
    // uint16_t subtraction underflow is well defined.
    for (uint16_t i = 0; i < static_cast<uint16_t>(message->ack_bits.size()); ++i) {
        if (message->ack_bits.test(i)) {
            uint16_t seq = message->ack_num - i;
            auto it = session.retransmit_queue.find(seq);
            if (it != session.retransmit_queue.end()) {
                if (it->second.attempts == 0) {
                    const uint64_t rtt_ns = session.last_receive_timestamp - it->second.original_timestamp;
                    session.rto_calc.recordRTT(std::chrono::nanoseconds(rtt_ns));
                    GC_DEBUG("New RTT: {} ms, RTO: {} ms", static_cast<double>(rtt_ns) / 1.0e6,
                             static_cast<double>(session.rto_calc.getRTONanoseconds()) / 1.0e6);
                }
                session.retransmit_queue.erase(it);
            }
        }
    }

    // read the data...
    if (reader.remaining() != message->payload_size) {
        GC_ERROR("Remaining packet size not equal to payload size");
        return std::nullopt; // malformed packet
    }

    GC_DEBUG("Received {} bytes:", message->payload_size);
    GC_DEBUG("  Message: seq_num: {}, ack_num: {}", message->seq_num, message->ack_num);

    if (reader.remaining() >= sizeof(uint32_t) && message->payload_type == 1) {
        const uint32_t hash = reader.readU32();
        NetEvent ev{};
        ev.type = Name(hash);
        ev.data.resize(reader.remaining());
        reader.readBytes(ev.data);
        return ev;
    }
    else {
        return std::nullopt;
    }
}

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    disconnect(); // just in case

    m_server_endpoint = endpoint;

    asio::error_code ec{};
    m_socket.open(endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        return false;
    }
    m_socket.connect(endpoint, ec);
    if (ec) {
        GC_ERROR("Failed to connect to server endpoint: {}", ec.message());
        return false;
    }
    GC_INFO("Connecting to server: {}", endpoint);

    m_client_thread = std::jthread(
        [](NetClient& self) {
            self.m_state.store(NetClientConnectionStatus::CONNECTING);
            self.m_session.session_token = 0;
            self.m_context.restart();
            asio::co_spawn(self.m_context, initiateConnection(self.m_socket, self.m_session), asio::detached);
            self.m_context.run(); // returns when initiateConnection completes
            if (self.m_session.session_token != 0) {
                self.m_state.store(NetClientConnectionStatus::CONNECTED);
                asio::co_spawn(self.m_context, self.sendLoop(), asio::detached);
                asio::co_spawn(self.m_context, self.receiveLoop(), asio::detached);
                asio::co_spawn(self.m_context, self.keepAliveLoop(), asio::detached);
                self.m_context.restart();
                self.m_context.run();
            }
            self.m_state.store(NetClientConnectionStatus::DISCONNECTED);
        },
        std::ref(*this));

    return true;
}

void NetClient::disconnect()
{
    asio::error_code ec{};
    m_context.stop();
    m_client_thread = {};
    m_socket.shutdown(asio::ip::udp::socket::shutdown_both, ec);
    (void)ec; // it errors if the socket hasn't been used at all yet
    m_socket.close();
    GC_ASSERT(getConnectionStatus() == NetClientConnectionStatus::DISCONNECTED);
}

bool NetClient::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

NetClientConnectionStatus NetClient::getConnectionStatus() const { return m_state.load(); }

asio::ip::udp::endpoint NetClient::getServerEndpoint() const { return m_server_endpoint; }

void NetClient::sendMessage(uint16_t payload_type, std::vector<uint8_t> payload)
{
    pushToOutboundQueue(OutboundMessage{.payload_type = payload_type, .payload = std::move(payload)});
}

void NetClient::pushToOutboundQueue(OutboundCommand command)
{
    asio::post(m_context, [this, command = std::move(command)] {
        if (!m_outbound_queue.try_send(asio::error_code{}, std::move(command))) {
            abortGame("NetClient outbound queue full! Capacity = {}", OUTBOUND_QUEUE_MAX_SIZE);
        }
    });
}

asio::awaitable<void> NetClient::sendLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::error_code ec{};

    GC_ASSERT(m_socket.is_open());

    OutboundCommand command{};
    std::array<uint8_t, NET_MAX_PACKET_SIZE> buffer{};
    ByteWriter writer(buffer);
    for (;;) {
        std::tie(ec, command) = co_await m_outbound_queue.async_receive(TOKEN);
        if (ec) {
            GC_ERROR("Outbound channel receive error: {}", ec.message());
            continue;
        }

        writer.reset();
        const uint64_t now = SDL_GetTicksNS();
        std::visit(overloaded{[&](const OutboundMessage& outbound) {
                                  NetPacketMessage message{};
                                  message.seq_num = m_session.next_seq_num;
                                  message.ack_num = m_session.last_ack_num;
                                  message.ack_bits = m_session.ack_bits;
                                  message.payload_type = outbound.payload_type;
                                  message.payload_size = static_cast<uint16_t>(outbound.payload.size());
                                  writePacketWithHeader(writer, m_session.session_token, message);
                                  GC_ASSERT(writer.remaining() >= message.payload_size);
                                  writer.writeBytes(outbound.payload);

                                  NetClientSession::QueuedPacket retransmit_packet{};
                                  retransmit_packet.original_timestamp = now;
                                  retransmit_packet.last_send_timestamp = now;
                                  retransmit_packet.attempts = 0;
                                  retransmit_packet.packet_data = std::make_shared<std::vector<uint8_t>>(buffer.cbegin(), buffer.cbegin() + writer.pos());
                                  m_session.retransmit_queue.emplace(message.seq_num, std::move(retransmit_packet));

                                  m_session.last_send_timestamp = now;
                                  m_session.next_seq_num += 1;

                                  GC_DEBUG("Sending message: seq_num: {}, ack_num: {}", message.seq_num, message.ack_num);
                              },
                              [&](const OutboundRaw& raw) {
                                  m_session.last_send_timestamp = now;
                                  GC_ASSERT(raw.packet_data);
                                  writer.writeBytes(std::span<const uint8_t>(raw.packet_data->data(), raw.packet_data->size()));
                              }},
                   command);

        if (writer.pos() == 0) {
            continue;
        }

        size_t bytes_written{};
        std::tie(ec, bytes_written) = co_await m_socket.async_send(asio::buffer(buffer.data(), writer.pos()), TOKEN);
        if (ec) {
            GC_ERROR("Failed to send from socket: {}", ec.message());
        }
        else if (bytes_written != writer.pos()) {
            GC_ERROR("Failed to send all data from socket. {}/{} bytes", bytes_written, writer.pos());
        }
    }
}

asio::awaitable<void> NetClient::receiveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

    GC_ASSERT(m_socket.is_open());

    std::array<uint8_t, NET_MAX_PACKET_SIZE> recv_buf{};
    for (;;) {
        size_t bytes_read{};
        std::tie(ec, bytes_read) = co_await m_socket.async_receive(asio::buffer(recv_buf), TOKEN);
        if (ec) {
            GC_ERROR("Failed to receive on socket: {}", ec.message());
            continue;
        }

        ByteReader reader(std::span(recv_buf.data(), bytes_read));

        const auto header = tryDeserialise<NetPacketHeader>(reader);
        if (!header || !verifyPacketHeader(*header)) {
            continue;
        }

        if (header->type != NetPacketType::MESSAGE || header->token != m_session.session_token) {
            continue;
        }

        const uint64_t now = SDL_GetTicksNS();

        m_session.last_receive_timestamp = now;

        if (auto ev = handleMessage(reader, m_session); ev) {
            m_event_queue.push(*ev);
        }
    }
}

// periodically checks session to see if it has timed out and removes it if so.
// Also pings the server if currently idle (no data received or sent recently).
// This lets the server check that the connection is still alive, and tells the client that the server is still active.
// Also retransmits packets that have not been acked.
asio::awaitable<void> NetClient::keepAliveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

    constexpr auto TIME_PERIOD = std::chrono::milliseconds(100); // 10 hz
    constexpr int64_t TIMEOUT_TIME_NS = 5'000'000'000LL;         // 5 s
    constexpr int64_t KEEPALIVE_IDLE_TIME_NS = 500'000'000LL;    // 500 ms

    for (;;) {
        // maybe timer can be reset every iteration instead of reconstructing?
        asio::steady_timer timer(executor, TIME_PERIOD);

        const int64_t now = SDL_GetTicksNS();

        std::vector<uint16_t> to_retransmit{};
        for (auto it = m_session.retransmit_queue.begin(); it != m_session.retransmit_queue.end();) {
            auto& [seq_num, packet] = *it;
            if (packet.attempts >= packet.MAX_ATTEMPTS || seq_diff(seq_num, m_session.next_seq_num) < -static_cast<int16_t>(m_session.ack_bits.size() * 2)) {
                GC_WARN("Queued packet {} was never acknowledged and is getting dropped: attempts: {}, age: {} ms", seq_num, packet.attempts,
                        static_cast<double>(now - packet.original_timestamp) / 1e6);
                it = m_session.retransmit_queue.erase(it);
            }
            else {
                if (now - packet.last_send_timestamp > static_cast<uint64_t>(m_session.rto_calc.getRTONanoseconds())) {
                    to_retransmit.push_back(seq_num);
                }
                ++it;
            }
        }

        for (const uint16_t seq_num : to_retransmit) {
            auto it = m_session.retransmit_queue.find(seq_num);
            if (it == m_session.retransmit_queue.end()) {
                continue; // packet was acked by receiveLoop() during the below co_await
            }
            auto& packet = it->second;

            // retransmit
            packet.last_send_timestamp = now;
            packet.attempts += 1;

            OutboundCommand command{};
            command.emplace<OutboundRaw>(packet.packet_data); // increases shared_ptr refcount

            GC_DEBUG("Retransmitting packet with seq_num: {}, attempt: {}", seq_num, packet.attempts);

            std::tie(ec) = co_await m_outbound_queue.async_send(asio::error_code{}, std::move(command), TOKEN);
            if (ec) {
                GC_ERROR("Outbound channel send error: {}", ec.message());
                continue;
            }
        }

        {
            const int64_t time_since_receive = now - static_cast<int64_t>(m_session.last_receive_timestamp);
            const int64_t time_since_send = now - static_cast<int64_t>(m_session.last_send_timestamp);
            if (time_since_receive > TIMEOUT_TIME_NS) {
                GC_WARN("Server timed out");
                m_context.stop();
                // Stopping the context is safe to do inside a completion handler.
                // This coroutine will exit via early return of the below timer.async_wait() call
            }
            else {
                // also sends ping if a packet hasn't been sent since the last receive (to ensure somewhat timely ACKs)
                if (time_since_receive > KEEPALIVE_IDLE_TIME_NS || time_since_send > KEEPALIVE_IDLE_TIME_NS ||
                    m_session.last_receive_timestamp > m_session.last_send_timestamp) {
                    // send keepalive packet with no data
                    OutboundCommand command{};
                    auto& message = command.emplace<OutboundMessage>();
                    message.payload_type = 0;
                    message.payload = {};

                    std::tie(ec) = co_await m_outbound_queue.async_send(asio::error_code{}, std::move(command), TOKEN);
                    if (ec) {
                        GC_ERROR("Outbound channel send error: {}", ec.message());
                        continue;
                    }
                }
            }
        }

        std::tie(ec) = co_await timer.async_wait(TOKEN);
        if (ec) {
            GC_ERROR("Timer error: {}", ec.message());
            co_return;
        }
    }
}

} // namespace gc
