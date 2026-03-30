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

static NetSessionToken computeSessionToken(uint64_t server_secret, const asio::ip::udp::endpoint& client_endpoint, uint64_t client_nonce, uint32_t time_bucket)
{
    // TODO
    // THIS IS NOT SECURE AT ALL!
    // USE SOMETHING LIKE BLAKE3 INSTEAD
    //
    // it also allocates on the heap, which the server code SHOULD NOT DO prior to client authentication
    std::stringstream data;
    data << server_secret;
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

static uint64_t generateServerSecret()
{
    std::mt19937_64 rand64(std::random_device{}());
    return rand64(); // TODO use cryptographically secure PRNG
}

struct PacketContext {
    const asio::ip::udp::endpoint& endpoint;
    const NetPacketHeader& received_header;
    NetByteReader& reader;
    NetByteWriter& writer;
    uint64_t server_secret;
    uint32_t time_bucket;
};

struct Session {
    asio::ip::udp::endpoint endpoint;
    uint32_t time_bucket_last_received;
};

// returns true on success.
// func has signature: void(T packet);
template <typename T, typename Func>
static void handleParsed(PacketContext& ctx, Func&& func)
{
    const auto pkt = tryDeserialiseExact<T>(ctx.reader);
    if (!pkt) {
        return;
    }
    func(std::move(*pkt));
}

static void handleUnauthenticated(PacketContext& ctx)
{
    // Unauthenticated packets are handled statelessly
    if (ctx.received_header.type != NetPacketType::CONNECT_REQUEST) {
        return;
    }
    handleParsed<NetPacketConnectRequest>(ctx, [&](const auto request) {
        const auto session_token = computeSessionToken(ctx.server_secret, ctx.endpoint, request.client_nonce, ctx.time_bucket);
        writePacketWithHeader(ctx.writer, session_token, NetPacketConnectChallenge{.client_nonce = request.client_nonce});
    });
}

static void handlePing(PacketContext& ctx)
{
    handleParsed<NetPacketPing>(ctx,
                                [&](const auto ping) { writePacketWithHeader(ctx.writer, ctx.received_header.token, NetPacketPong{.seq_num = ping.seq_num}); });
}

static void handleAuthenticated(PacketContext& ctx, std::unordered_map<NetSessionToken, Session>& sessions)
{
    // verify token
    const auto it = sessions.find(ctx.received_header.token);
    if (it == sessions.end()) {
        // token not found, might be a new valid session...
        if (ctx.received_header.type != NetPacketType::CONNECT_CHALLENGE_RESPONSE) {
            return;
        }
        handleParsed<NetPacketConnectChallengeResponse>(ctx, [&](const auto challenge_response) {
            // Also compare against what the session token would have been during the previous time bucket.
            const auto session_token1 = computeSessionToken(ctx.server_secret, ctx.endpoint, challenge_response.client_nonce, ctx.time_bucket);
            const auto session_token2 = computeSessionToken(ctx.server_secret, ctx.endpoint, challenge_response.client_nonce, ctx.time_bucket - 1);
            if (ctx.received_header.token != session_token1 && ctx.received_header.token != session_token2) {
                return;
            }
            Session session{};
            session.endpoint = ctx.endpoint;
            session.time_bucket_last_received = ctx.time_bucket;
            sessions.emplace(ctx.received_header.token, std::move(session));
            GC_DEBUG("Created new session");
        });
        return;
    }

    Session& session = it->second;

    // Verify session has same endpoint
    if (session.endpoint != ctx.endpoint) {
        // Might just be a NAT rebind or carrier handoff.
        // TODO: Handle endpoint migration
        GC_ERROR("Packet from {} has session token corresponding to existing session with {}", ctx.endpoint, session.endpoint);
        return;
    }

    // Authenticated session
    session.time_bucket_last_received = ctx.time_bucket;

    switch (ctx.received_header.type) {
    case NetPacketType::PING:
        handlePing(ctx);
        break;
    default:
        break;
    }
}

static bool isSessionExpired(const Session& session, uint32_t current_time_bucket)
{
    // a time bucket is 10 seconds, so a session is expired if its been at least 10-20 seconds.
    return static_cast<uint32_t>(current_time_bucket - session.time_bucket_last_received) >= 2; // handles wrap around
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
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

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
        GC_INFO("Started server on {}", m_server_status.local_endpoint);
    }

    const uint64_t server_secret = generateServerSecret();

    std::unordered_map<NetSessionToken, Session> sessions{};

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
        NetByteWriter writer(send_buf);

        const auto header = tryDeserialise<NetPacketHeader>(reader);
        if (!header || !verifyPacketHeader(*header)) {
            continue;
        }

        PacketContext ctx{.endpoint = client_endpoint, //
                          .received_header = *header,
                          .reader = reader,
                          .writer = writer,
                          .server_secret = server_secret,
                          .time_bucket = time_bucket};

        if (header->token == NetSessionToken{}) {
            handleUnauthenticated(ctx);
        }
        else {
            handleAuthenticated(ctx, sessions);
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

        // remove expired sessions
        if (size_t num_erased = std::erase_if(sessions, [&](const auto& entry) { return isSessionExpired(entry.second, time_bucket); }); num_erased > 0) {
            GC_DEBUG("removed {} sessions", num_erased);
        }
    }
}

} // namespace gc
