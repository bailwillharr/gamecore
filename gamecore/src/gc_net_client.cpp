#include "gamecore/gc_net_client.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <chrono>

#include "gamecore/gc_net_common.h"

namespace gc {

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    disconnect(); // just in case
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
    m_state.store(NetClientState::DISCONNECTED);
}

bool NetClient::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

NetClientState NetClient::getState() const
{
    return m_state.load(); // not sure if relaxed can be used here since this flag can cause the client to be destroyed
}

asio::awaitable<void> NetClient::clientLoop(asio::ip::udp::endpoint server_endpoint)
{
    using namespace asio::experimental::awaitable_operators;

    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);

    constexpr auto TIMEOUT = std::chrono::milliseconds(500);
    constexpr int MAX_RETRIES = 3;

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    GC_INFO("Connecting to server: {}:{}", server_endpoint.address().to_string(), server_endpoint.port());

    asio::ip::udp::socket socket(executor);

    socket.open(server_endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        co_return;
    }

    // this doesn't really 'connect', but it sets up send() and receive() to only use this endpoint
    std::tie(ec) = co_await socket.async_connect(server_endpoint, TOKEN);
    if (ec) {
        GC_ERROR("Failed to connect socket: {}", ec.message());
        co_return;
    }

    // send a CONNECT_REQUEST, wait for a CONNECT_CHALLENGE, retry a few times if it times out

    std::array<uint8_t, NET_MAX_PACKET_SIZE> receive_buf{};
    size_t bytes_received = 0;
    NetByteReader reader(receive_buf);

    for (int i = 0; i < MAX_RETRIES; ++i) {
        {
            auto header = NetPacketHeader::createValid(NetPacketType::CONNECT_REQUEST);
            auto connect_request = NetPacketConnectRequest{};
            connect_request.client_nonce = static_cast<uint64_t>(rand()); // TODO
            std::array<uint8_t, NET_MAX_PACKET_SIZE> buf{};
            NetByteWriter writer(buf);
            header.serialise(writer);
            connect_request.serialise(writer);
            std::tie(ec, std::ignore) = co_await socket.async_send(asio::buffer(buf.data(), writer.pos()), TOKEN);
            if (ec) {
                GC_ERROR("Failed to send on socket: {}", ec.message());
                co_return;
            }
        }
        {
            asio::steady_timer timer(executor, TIMEOUT);

            auto results = co_await (socket.async_receive(asio::buffer(receive_buf), TOKEN) || timer.async_wait(TOKEN));
            if (results.index() == 1) {
                std::tie(ec) = std::get<1>(results);
                if (ec) {
                    GC_ERROR("Timeout error: {}", ec.message());
                    co_return;
                }
                GC_WARN("Timeout connecting to {}:{}", server_endpoint.address().to_string(), server_endpoint.port());
                continue;
            }
            else {
                std::tie(ec, bytes_received) = std::get<0>(results);
                if (ec) {
                    GC_ERROR("socket receive error: {}", ec.message());
                    co_return;
                }
                if (bytes_received > 0) {
                    break; // RECEIVED PACKET
                }
            }
        }
    }

    if (bytes_received < NetPacketHeader::getSerialisedSize() + NetPacketConnectChallenge::getSerialisedSize()) {
        GC_ERROR("Could not connect to server");
        co_return;
    }

    const auto challenge_header = NetPacketHeader::deserialise(reader);
    if (!verifyPacketHeader(challenge_header) || challenge_header.type != NetPacketType::CONNECT_CHALLENGE) {
        GC_ERROR("Invalid packet from server");
        co_return;
    }

    const auto challenge = NetPacketConnectChallenge::deserialise(reader);

    {
        auto header = NetPacketHeader::createValid(NetPacketType::CONNECT_CHALLENGE_RESPONSE, challenge_header.token);
        std::array<uint8_t, NET_MAX_PACKET_SIZE> buf{};
        NetByteWriter writer(buf);
        header.serialise(writer);
        auto challenge_response = NetPacketConnectChallengeResponse{};
        challenge_response.client_nonce = challenge.client_nonce;
        challenge_response.serialise(writer);
        std::tie(ec, std::ignore) = co_await socket.async_send(asio::buffer(buf.data(), writer.pos()), TOKEN);
        if (ec) {
            GC_ERROR("Failed to send on socket: {}", ec.message());
            co_return;
        }
    }

    NetSessionToken session_token = challenge_header.token;

    m_state.store(NetClientState::CONNECTED);

    uint32_t seq_num = 0;
    for (;;) {
        {
            asio::steady_timer timer(executor, std::chrono::seconds(1));
            std::tie(ec) = co_await timer.async_wait(TOKEN);
            if (ec) {
                GC_ERROR("Timeout error: {}", ec.message());
                co_return;
            }
        }

        std::chrono::steady_clock::duration rtt{};
        for (int i = 0; i < MAX_RETRIES; ++i) {
            ++seq_num;

            auto header = NetPacketHeader::createValid(NetPacketType::PING, session_token);
            auto ping = NetPacketPing{};
            ping.seq_num = seq_num;
            std::array<uint8_t, NET_MAX_PACKET_SIZE> buf{};
            NetByteWriter writer(buf);
            header.serialise(writer);
            ping.serialise(writer);

            const auto ping_send_time = std::chrono::steady_clock::now();

            std::tie(ec, std::ignore) = co_await socket.async_send(asio::buffer(buf.data(), writer.pos()), TOKEN);
            if (ec) {
                GC_ERROR("Failed to send on socket: {}", ec.message());
                co_return;
            }

            asio::steady_timer timer(executor, TIMEOUT);
            auto results = co_await (socket.async_receive(asio::buffer(receive_buf), TOKEN) || timer.async_wait(TOKEN));

            const auto pong_recv_time = std::chrono::steady_clock::now();
            rtt = pong_recv_time - ping_send_time;

            bytes_received = 0;
            if (results.index() == 1) {
                std::tie(ec) = std::get<1>(results);
                if (ec) {
                    GC_ERROR("Timeout error: {}", ec.message());
                    co_return;
                }
                GC_WARN("Timeout pinging {}:{}", server_endpoint.address().to_string(), server_endpoint.port());
                continue;
            }
            else {
                std::tie(ec, bytes_received) = std::get<0>(results);
                if (ec) {
                    GC_ERROR("socket receive error: {}", ec.message());
                    co_return;
                }
                if (bytes_received > 0) {
                    break; // RECEIVED PACKET
                }
            }
        }

        reader.reset();
        if (bytes_received >= NetPacketHeader::getSerialisedSize() + NetPacketPong::getSerialisedSize()) {
            if (const auto header = NetPacketHeader::deserialise(reader); header.type == NetPacketType::PONG && header.token == session_token) {
                const auto pong = NetPacketPong::deserialise(reader);
                if (seq_num == pong.seq_num) {
                    // received pong
                    // determine RTT here.
                    GC_INFO("RTT: {}", std::chrono::duration_cast<std::chrono::milliseconds>(rtt));
                    continue;
                }
            }
        }

        // didn't receive pong, lost connection to server
        co_return;
    }
}

} // namespace gc
