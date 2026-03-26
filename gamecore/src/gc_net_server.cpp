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

static NetServerToken createToken(uint64_t secret, const asio::ip::udp::endpoint& client_endpoint, uint64_t client_nonce, uint32_t time_bucket)
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

    NetServerToken token{};

    for (int i = 0; i < token.size(); ++i) {
        token[i] = (hash >> ((i % 4) * 8)) & 0xFF;
    }

    return token;
}

static bool verifyPacketHeader(const NetPacketHeader& header)
{
    if (header.magic != NET_PACKET_MAGIC) {
        return false;
    }
    if (header.version != NET_PACKET_VERSION) {
        return false;
    }
    if (header.type >= NetPacketType::INVALID) {
        return false;
    }
    return true;
}

static NetPacketConnectChallenge createChallengePacket(const NetPacketConnectRequest& request, uint64_t secret, const asio::ip::udp::endpoint& client_endpoint,
                                                       uint32_t time_bucket)
{
    NetPacketConnectChallenge challenge{};
    challenge.token = createToken(secret, client_endpoint, request.client_nonce, time_bucket);
    challenge.client_nonce = request.client_nonce;
    challenge.time_bucket = time_bucket;
    return challenge;
}

static bool checkChallengeResponse(const NetPacketConnectChallenge& response, uint64_t secret, const asio::ip::udp::endpoint& client_endpoint)
{
    const auto real_token = createToken(secret, client_endpoint, response.client_nonce, response.time_bucket);
    return response.token == real_token;
}

NetServer::~NetServer() { stop(); }

bool NetServer::start(asio::ip::udp::endpoint endpoint)
{
    m_server_thread = std::jthread(
        [](NetServer& self, asio::ip::udp::endpoint endpoint) {
            asio::co_spawn(self.m_context, self.serverLoop(endpoint), asio::detached);
            self.m_context.run();
        },
        std::ref(*this), std::move(endpoint));

    return true;
}

void NetServer::stop() { m_context.stop(); }

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

    GC_INFO("Starting server on {}:{}", endpoint.address().to_string(), endpoint.port());

    std::mt19937_64 rand64(std::random_device{}());
    const uint64_t secret = rand64(); // TODO VERY INSECURE AND BAD

    for (;;) {
        size_t bytes_read{};
        std::array<uint8_t, NET_MAX_PACKET_SIZE> recv_buf{};
        asio::ip::udp::endpoint client_endpoint{};
        std::tie(ec, bytes_read) = co_await socket.async_receive_from(asio::buffer(recv_buf), client_endpoint, TOKEN);
        if (ec) {
            GC_ERROR("Failed to receive on socket: {}", ec.message());
            continue;
        }

        const uint32_t time_bucket = getTimeBucket();

        NetByteReader reader(std::span(recv_buf.data(), bytes_read));
        if (reader.remaining() < NetPacketHeader::getSerialisedSize()) {
            GC_DEBUG("Received packet too small");
            continue;
        }

        auto header = NetPacketHeader::deserialise(reader);
        if (!verifyPacketHeader(header)) {
            GC_DEBUG("Received packet has invalid header");
            continue;
        }

        GC_DEBUG("Incoming packet type: {}", static_cast<uint32_t>(header.type));

        std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf;
        NetByteWriter writer(send_buf);
        switch (header.type) {
        case NetPacketType::CONNECT_REQUEST: {
            if (reader.remaining() < NetPacketConnectRequest::getSerialisedSize()) {
                GC_DEBUG("incoming connect request packet has invalid size");
                continue;
            }
            GC_DEBUG("Creating challenge packet");
            NetPacketHeader::createValid(NetPacketType::CONNECT_CHALLENGE).serialise(writer);
            createChallengePacket(NetPacketConnectRequest::deserialise(reader), secret, client_endpoint, time_bucket).serialise(writer);
        } break;
        case NetPacketType::CONNECT_CHALLENGE_RESPONSE: {
            if (reader.remaining() < NetPacketConnectChallenge::getSerialisedSize()) {
                continue;
            }
            if (checkChallengeResponse(NetPacketConnectChallenge::deserialise(reader), secret, client_endpoint)) {
                GC_INFO("Successfully verified connection");
            }
        } break;
        default:
            continue;
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
            GC_DEBUG("Sent packet of size: {} to {}:{}", writer.pos(), client_endpoint.address().to_string(), client_endpoint.port());
        }
    }
}

} // namespace gc
