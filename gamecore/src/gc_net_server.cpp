#include "gamecore/gc_net_server.h"

#include <chrono>
#include <random>

#include <asio/awaitable.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/co_spawn.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>
#include <asio/as_tuple.hpp>
#include <asio/streambuf.hpp>

#include "gamecore/gc_asio_throw_exception.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_net_common.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_crc_table.h"

namespace gc {

static uint32_t getTimeBucket()
{
    using namespace std::chrono_literals;
    constexpr auto BUCKET_SIZE = 10s;
    auto bucket = std::chrono::steady_clock::now().time_since_epoch() / BUCKET_SIZE;
    return static_cast<uint32_t>(bucket); // truncates uint64_t to uint32_t
}

static NetSessionToken createToken(uint64_t secret, const asio::ip::udp::endpoint& client_endpoint, uint64_t client_nonce, uint32_t time_bucket)
{
    // TODO
    // THIS IS NOT SECURE AT ALL!
    // USE SOMETHING LIKE BLAKE3 INSTEAD
    //
    // it also allocates on the heap, which the server code SHOULD NOT DO prior to client authentication
    std::stringstream data;
    data << secret;
    data << '|';
    data << client_endpoint.address().to_string();
    data << '|';
    data << client_endpoint.port();
    data << '|';
    data << client_nonce;
    data << '|';
    data << time_bucket;

    uint32_t hash = crc32_impl(data.str().c_str());

    NetSessionToken token{};

    for (int i = 0; i < token.size(); ++i) {
        token[i] = (hash >> ((i % 4) * 8)) & 0xFF;
    }

    return token;
}

NetServer::~NetServer() { stop(); }

bool NetServer::start(asio::ip::udp::endpoint endpoint)
{
    stop(); // just in case
    m_context.restart();
    m_server_thread = std::jthread(
        [](NetServer& self, asio::ip::udp::endpoint endpoint) {
            asio::co_spawn(self.m_context, self.serverLoop(endpoint), asio::detached);
            self.m_context.run();
        },
        std::ref(*this), std::move(endpoint));

    return true;
}

void NetServer::stop()
{
    m_context.stop();
    m_server_thread = {};
}

bool NetServer::isRunning() const { return !m_context.stopped(); }

asio::ip::udp::endpoint NetServer::getLocalEndpoint() const
{
    std::scoped_lock lock(m_server_status.mutex);
    return m_server_status.local_endpoint;
}

asio::awaitable<void> NetServer::serverLoop(asio::ip::udp::endpoint endpoint)
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    asio::ip::udp::socket socket(executor);

    socket.open(endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        co_return;
    }

    socket.bind(endpoint, ec);
    if (ec) {
        GC_ERROR("Failed to bind socket: {}", ec.message());
        co_return;
    }

    {
        std::scoped_lock lock(m_server_status.mutex);
        m_server_status.local_endpoint = socket.local_endpoint();
        GC_INFO("Starting server on {}:{}", m_server_status.local_endpoint.address().to_string(), m_server_status.local_endpoint.port());
    }

    std::mt19937_64 rand64(std::random_device{}());
    const uint64_t secret = rand64(); // TODO use cryptographically secure PRNG

    struct Session {
        NetSessionToken token;
    };
    std::unordered_map<asio::ip::udp::endpoint, Session> sessions{};

    std::array<uint8_t, NET_MAX_PACKET_SIZE> recv_buf{};
    std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf{};
    for (;;) {
        size_t bytes_read{};
        asio::ip::udp::endpoint client_endpoint{};
        std::tie(ec, bytes_read) = co_await socket.async_receive_from(asio::buffer(recv_buf), client_endpoint, TOKEN);
        if (ec) {
            GC_ERROR("Failed to receive on socket: {}", ec.message());
            continue;
        }

        const uint32_t time_bucket = getTimeBucket();

        NetByteReader reader(std::span(recv_buf.data(), bytes_read));
        if (reader.remaining() < NetPacketHeader::getSerialisedSize()) {
            continue;
        }

        auto header = NetPacketHeader::deserialise(reader);
        if (!verifyPacketHeader(header)) {
            GC_DEBUG("Received packet has invalid header");
            continue;
        }

        NetByteWriter writer(send_buf);

        if (header.token.empty()) {
            // Unauthenticated packets are handled statelessly
            if (header.type != NetPacketType::CONNECT_REQUEST) {
                continue;
            }
            if (reader.remaining() < NetPacketConnectRequest::getSerialisedSize()) {
                continue;
            }
            const auto request = NetPacketConnectRequest::deserialise(reader);
            const auto session_token = createToken(secret, client_endpoint, request.client_nonce, time_bucket);
            NetPacketHeader::createValid(NetPacketType::CONNECT_CHALLENGE, session_token).serialise(writer);
            NetPacketConnectChallenge{.client_nonce = request.client_nonce, .time_bucket = time_bucket}.serialise(writer);
        }
        else {
            // verify token
            const auto it = sessions.find(client_endpoint);
            if (it == sessions.end()) {
                // token not found, might be a new valid session...
                if (header.type != NetPacketType::CONNECT_CHALLENGE_RESPONSE) {
                    continue;
                }
                if (reader.remaining() < NetPacketConnectChallenge::getSerialisedSize()) {
                    continue;
                }
                const auto challenge_response = NetPacketConnectChallenge::deserialise(reader);
                const auto real_session_token = createToken(secret, client_endpoint, challenge_response.client_nonce, challenge_response.time_bucket);
                if (header.token != real_session_token) {
                    continue;
                }
                sessions.emplace(client_endpoint, real_session_token);
                continue;
            }
            else if (header.token != it->second.token) {
                continue; // invalid token
            }

            // Authenticated session
            switch (header.type) {
            case NetPacketType::PING: {
                if (reader.remaining() < NetPacketPing::getSerialisedSize()) {
                    continue;
                }
                const auto ping = NetPacketPing::deserialise(reader);
                NetPacketHeader::createValid(NetPacketType::PONG, header.token).serialise(writer);
                NetPacketPong{.seq_num = ping.seq_num}.serialise(writer);
            } break;
            default:
                break;
            }
        }

        if (writer.pos() > 0) {
            // send packet
            size_t bytes_written{};
            std::tie(ec, bytes_written) = co_await socket.async_send_to(asio::buffer(std::span(send_buf.data(), writer.pos())), client_endpoint, TOKEN);
            if (ec) {
                GC_ERROR("Failed to send from socket: {}", ec.message());
                continue;
            }
            else if (bytes_written != writer.pos()) {
                GC_ERROR("Failed to send all data from socket. {}/{} bytes", bytes_written, writer.pos());
                continue;
            }
        }
    }
}

} // namespace gc
