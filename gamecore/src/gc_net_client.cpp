#include "gamecore/gc_net_client.h"

#include <random>
#include <chrono>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"

namespace gc {

static uint32_t generateClientNonce()
{
    std::mt19937 rand32(std::random_device{}());
    return rand32();
}

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    disconnect();
    m_context.restart();
    m_state.store(NetClientState::CONNECTING);
    m_client_thread = std::jthread(
        [](NetClient& self, asio::ip::udp::endpoint endpoint) {
            asio::co_spawn(self.m_context, self.clientLoop(endpoint), asio::detached);
            self.m_context.run();
            self.m_state.store(NetClientState::DISCONNECTED);
        },
        std::ref(*this), endpoint);
    return true;
}

void NetClient::disconnect()
{
    m_context.stop();
    m_client_thread = {};
    m_socket.reset();
    m_session_token.reset();
    m_recv_watchdog.reset();
    m_state.store(NetClientState::DISCONNECTED);
}

bool NetClient::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

NetClientState NetClient::getState() const { return m_state.load(); }

asio::awaitable<bool> NetClient::handshake(asio::ip::udp::endpoint server_endpoint)
{
    using namespace asio::experimental::awaitable_operators;
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    constexpr int MAX_ATTEMPTS = 4;

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    GC_INFO("Connecting to server: {}", server_endpoint);

    m_socket.emplace(executor);
    m_socket->open(server_endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        co_return false;
    }

    std::tie(ec) = co_await m_socket->async_connect(server_endpoint, TOKEN);
    if (ec) {
        GC_ERROR("Failed to connect socket: {}", ec.message());
        co_return false;
    }

    auto current_timeout = std::chrono::milliseconds(1000);
    std::array<uint8_t, NET_MAX_PACKET_SIZE> receive_buf{};

    const uint64_t client_nonce = static_cast<uint64_t>(generateClientNonce());

    size_t bytes_received = 0;
    for (int i = 0; i < MAX_ATTEMPTS; ++i) {
        {
            std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf{};
            NetByteWriter writer(send_buf);
            NetPacketHeader::createValid(NetPacketType::CONNECT_REQUEST).serialise(writer);
            NetPacketConnectRequest{.client_nonce = client_nonce}.serialise(writer);
            std::tie(ec, std::ignore) = co_await m_socket->async_send(asio::buffer(send_buf.data(), writer.pos()), TOKEN);
            if (ec) {
                GC_ERROR("Failed to send connect request: {}", ec.message());
                co_return false;
            }
        }

        asio::steady_timer timer(executor, current_timeout);
        auto results = co_await (m_socket->async_receive(asio::buffer(receive_buf), TOKEN) || timer.async_wait(TOKEN));
        if (results.index() == 1) {
            std::tie(ec) = std::get<1>(results);
            if (ec) {
                GC_ERROR("Handshake timer error: {}", ec.message());
                co_return false;
            }
            GC_WARN("Timeout connecting to {}", server_endpoint);
            current_timeout *= 2;
            continue;
        }

        std::tie(ec, bytes_received) = std::get<0>(results);
        if (ec) {
            GC_ERROR("Socket receive error: {}", ec.message());
            co_return false;
        }

        NetByteReader reader(std::span(receive_buf.data(), bytes_received));
        const auto header = tryDeserialise<NetPacketHeader>(reader);
        const auto challenge = tryDeserialiseExact<NetPacketConnectChallenge>(reader);
        if (!header || !verifyPacketHeader(*header) || !challenge || header->type != NetPacketType::CONNECT_CHALLENGE) {
            continue;
        }
        if (challenge->client_nonce != client_nonce) {
            continue;
        }

        {
            std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf{};
            NetByteWriter writer(send_buf);
            NetPacketHeader::createValid(NetPacketType::CONNECT_CHALLENGE_RESPONSE, header->token).serialise(writer);
            NetPacketConnectChallengeResponse{.client_nonce = client_nonce}.serialise(writer);
            std::tie(ec, std::ignore) = co_await m_socket->async_send(asio::buffer(send_buf.data(), writer.pos()), TOKEN);
            if (ec) {
                GC_ERROR("Failed to send challenge response: {}", ec.message());
                co_return false;
            }
        }

        m_session_token = header->token;
        co_return true;
    }

    co_return false;
}

asio::awaitable<void> NetClient::sendLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::error_code ec{};

    GC_ASSERT(m_socket);

    OutboundPacket packet{};
    for (;;) {
        std::tie(ec, packet) = co_await m_outbound_queue.async_receive(TOKEN);
        if (ec) {
            GC_ERROR("Outbound channel receive error: {}", ec.message());
            co_return;
        }

        size_t bytes_written{};
        std::tie(ec, bytes_written) = co_await m_socket->async_send(asio::buffer(packet.data), TOKEN);
        if (ec) {
            GC_ERROR("Failed to send packet: {}", ec.message());
            co_return;
        }
        if (bytes_written != packet.data.size()) {
            GC_ERROR("Failed to send full packet: {}/{}", bytes_written, packet.data.size());
            co_return;
        }
    }
}

asio::awaitable<void> NetClient::heartbeatLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    GC_ASSERT(m_session_token);

    uint16_t seq_num = 0;
    for (;;) {
        asio::steady_timer timer(executor, std::chrono::seconds(1));
        std::tie(ec) = co_await timer.async_wait(TOKEN);
        if (ec) {
            co_return;
        }

        std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf{};
        NetByteWriter writer(send_buf);
        NetPacketHeader::createValid(NetPacketType::PING, *m_session_token).serialise(writer);
        NetPacketPing{.seq_num = static_cast<uint16_t>(++seq_num)}.serialise(writer);

        OutboundPacket packet{};
        packet.data.assign(send_buf.data(), send_buf.data() + writer.pos());
        std::tie(ec) = co_await m_outbound_queue.async_send(asio::error_code{}, std::move(packet), TOKEN);
        if (ec) {
            GC_ERROR("Failed queueing heartbeat packet: {}", ec.message());
            co_return;
        }
    }
}

asio::awaitable<void> NetClient::receiveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    using namespace asio::experimental::awaitable_operators;

    asio::error_code ec{};

    GC_ASSERT(m_socket);
    GC_ASSERT(m_session_token);
    GC_ASSERT(m_recv_watchdog);

    std::array<uint8_t, NET_MAX_PACKET_SIZE> recv_buf{};
    for (;;) {
        m_recv_watchdog->expires_after(std::chrono::seconds(30));
        auto results = co_await (m_socket->async_receive(asio::buffer(recv_buf), TOKEN) || m_recv_watchdog->async_wait(TOKEN));

        if (results.index() == 1) {
            std::tie(ec) = std::get<1>(results);
            if (!ec) {
                GC_WARN("Connection timed out waiting for packets");
            }
            co_return;
        }

        size_t bytes_received{};
        std::tie(ec, bytes_received) = std::get<0>(results);
        if (ec) {
            GC_ERROR("Socket receive error: {}", ec.message());
            co_return;
        }

        NetByteReader reader(std::span(recv_buf.data(), bytes_received));
        const auto header = tryDeserialise<NetPacketHeader>(reader);
        if (!header || !verifyPacketHeader(*header) || header->token != *m_session_token) {
            continue;
        }

        switch (header->type) {
        case NetPacketType::PONG: {
            const auto pong = tryDeserialiseExact<NetPacketPong>(reader);
            if (!pong) {
                GC_WARN("Received invalid PONG packet");
            }
            break;
        }
        case NetPacketType::GAME_RELIABLE_HEADER:
        case NetPacketType::GAME_UNRELIABLE_HEADER:
            // TODO: forward application payloads to event queue once payload-carrying NetEvent is implemented
            break;
        default:
            break;
        }
    }
}

asio::awaitable<void> NetClient::clientLoop(asio::ip::udp::endpoint server_endpoint)
{
    using namespace asio::experimental::awaitable_operators;

    if (!(co_await handshake(server_endpoint))) {
        co_return;
    }

    m_state.store(NetClientState::CONNECTED);
    m_event_queue.push(NetEvent{.type = Name::createConstexpr("net_connected")});

    auto executor = co_await asio::this_coro::executor;
    m_recv_watchdog.emplace(executor);

    co_await (receiveLoop() || sendLoop() || heartbeatLoop());

    m_event_queue.push(NetEvent{.type = Name::createConstexpr("net_disconnected")});
    m_context.stop();
}

} // namespace gc
