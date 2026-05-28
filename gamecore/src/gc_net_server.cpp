#include "gamecore/gc_net_server.h"

#include <random>
#include <variant>

#include <SDL3/SDL_timer.h>

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
#include <asio/experimental/channel.hpp>

#include "gamecore/gc_asio_throw_exception.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_net_common.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_crc_table.h"

namespace gc {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
static overloaded(Ts...) -> overloaded<Ts...>;

static uint32_t getTimeBucket()
{
    const uint64_t bucket = SDL_GetTicksNS() / 10'000'000'000LL;
    return static_cast<uint32_t>(bucket); // truncates uint64_t to uint32_t
}

template <typename T>
static std::size_t simpleHash(const T& obj)
{
    const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(&obj);
    std::size_t hash = 1469598103934665603ull; // FNV offset basis

    for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull; // FNV prime
    }

    return hash;
}

static NetSessionToken computeSessionToken(uint64_t server_secret, const asio::ip::udp::endpoint& client_endpoint, uint64_t client_nonce, uint32_t time_bucket)
{
    // TODO
    // THIS IS NOT SECURE AT ALL!
    // USE SOMETHING LIKE BLAKE3 INSTEAD

    struct Data {
        uint64_t server_secret;
        std::array<uint8_t, 16> address;
        uint64_t client_nonce;
        uint32_t time_bucket;
        uint16_t port;
        uint16_t padding; // must be zero
    };
    static_assert(sizeof(Data) == 40);
    static_assert(std::endian::native == std::endian::little);
    static_assert(std::is_same_v<asio::ip::port_type, uint16_t>);

    Data data{};
    data.server_secret = server_secret;
    if (client_endpoint.address().is_v4()) {
        const auto bytes = client_endpoint.address().to_v4().to_bytes();
        std::copy(bytes.begin(), bytes.end(), data.address.begin());
    }
    else if (client_endpoint.address().is_v6()) {
        const auto bytes = client_endpoint.address().to_v6().to_bytes();
        std::copy(bytes.begin(), bytes.end(), data.address.begin());
    }
    else {
        GC_ASSERT(false);
    }
    data.client_nonce = client_nonce;
    data.time_bucket = time_bucket;
    data.port = client_endpoint.port();
    data.padding = 0;

    size_t hash = simpleHash(data);

    NetSessionToken token{};

    for (int i = 0; i < token.size(); ++i) {
        token[i] = (hash >> ((i % 8) * 8)) & 0xFF;
    }

    return token;
}

static uint64_t generateServerSecret()
{
    std::mt19937_64 rand64(std::random_device{}());
    return rand64(); // TODO use cryptographically secure PRNG
}

// returns positive: a is newer than b
// returns negative: a is older than b
static int16_t seq_diff(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b); }

struct PacketContext {
    const asio::ip::udp::endpoint& endpoint;
    const NetPacketHeader& received_header;
    NetByteReader& reader;
    const uint64_t server_secret;
    const uint32_t time_bucket;
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
    handleParsed<NetPacketConnectRequest>(ctx, [&](NetPacketConnectRequest request) {
        const auto session_token = computeSessionToken(ctx.server_secret, ctx.endpoint, request.client_nonce, ctx.time_bucket);
        writePacketWithHeader(ctx.writer, session_token, NetPacketConnectChallenge{.client_nonce = request.client_nonce});
    });
}

static void handleMessage(PacketContext& ctx, NetSession& session)
{
    const auto message = tryDeserialise<NetPacketMessage>(ctx.reader);
    if (!message) {
        return;
    }

    // read seq and ack information
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
        // previously missing sequence number, acknowledge it.
        session.ack_bits.set(-diff, true);
        read_message = true;
    }
    // outdated packets (old sequence numbers) are ignored

    // Remove newly acked outbound packets from queue...
    // uint16_t subtraction underflow is well defined.
    for (uint16_t i = 0; i < static_cast<uint16_t>(session.ack_bits.size()); ++i) {
        if (session.ack_bits.test(i)) {
            uint16_t seq = message->ack_num - i;
            auto it = session.retransmit_queue.find(seq);
            if (it != session.retransmit_queue.end()) {
                session.retransmit_queue.erase(it);
            }
        }
    }

    // read the data...
    if (ctx.reader.remaining() != message->payload_size) {
        GC_ERROR("Remaining packet size not equal to payload size");
        return; // malformed packet
    }

    // TODO: actually return data
    GC_TRACE("Session: {}, Recieved {} bytes", session.session_token, message->payload_size);
}

static void handleAuthenticated(PacketContext& ctx, std::unordered_map<NetSessionToken, NetSession>& sessions)
{
    // verify token

    const uint64_t now = SDL_GetTicksNS();

    const auto it = sessions.find(ctx.received_header.token);
    if (it == sessions.end()) {
        // token not found, might be a new valid session...
        if (ctx.received_header.type != NetPacketType::CONNECT_CHALLENGE_RESPONSE) {
            return;
        }
        handleParsed<NetPacketConnectChallengeResponse>(ctx, [&](NetPacketConnectChallengeResponse challenge_response) {
            // Also compare against what the session token would have been during the previous time bucket.
            const auto session_token1 = computeSessionToken(ctx.server_secret, ctx.endpoint, challenge_response.client_nonce, ctx.time_bucket);
            const auto session_token2 = computeSessionToken(ctx.server_secret, ctx.endpoint, challenge_response.client_nonce, ctx.time_bucket - 1);
            if (ctx.received_header.token != session_token1 && ctx.received_header.token != session_token2) {
                return;
            }
            NetSession session{};
            session.session_token = ctx.received_header.token;
            session.endpoint = ctx.endpoint;
            session.last_received_timestamp = now;
            sessions.emplace(ctx.received_header.token, std::move(session));
            GC_DEBUG("Created new session");
        });
        return;
    }

    NetSession& session = it->second;

    // Verify session has same endpoint
    if (session.endpoint != ctx.endpoint) {
        // Might just be a NAT rebind or carrier handoff.
        // TODO: Handle endpoint migration
        GC_ERROR("Packet from {} has session token corresponding to existing session with {}", ctx.endpoint, session.endpoint);
        return;
    }

    // Authenticated session
    session.last_received_timestamp = now;

    switch (ctx.received_header.type) {
    case NetPacketType::MESSAGE:
        handleMessage(ctx, session);
        break;
    default:
        // message is the only authenticated packet type
        break;
    }
}

NetServer::~NetServer() { stop(); }

bool NetServer::start(const asio::ip::udp::endpoint& endpoint)
{
    stop(); // just in case

    asio::error_code ec{};
    m_socket = asio::ip::udp::socket(m_context);
    m_socket->open(endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        return false;
    }
    m_socket->bind(endpoint, ec);
    if (ec) {
        GC_ERROR("Failed to bind socket: {}", ec.message());
        return false;
    }
    {
        std::scoped_lock lock(m_server_status.mutex);
        m_server_status.local_endpoint = m_socket->local_endpoint();
        GC_INFO("Starting server on {}", m_server_status.local_endpoint);
    }

    m_context.restart();
    m_server_thread = std::jthread(
        [](NetServer& self) {
            asio::co_spawn(self.m_context, self.receiveLoop(), asio::detached);
            asio::co_spawn(self.m_context, self.sendLoop(), asio::detached);
            asio::co_spawn(self.m_context, self.keepAliveLoop(), asio::detached);
            self.m_context.run();
        },
        std::ref(*this));

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

void NetServer::pushToOutboundQueue(OutboundCommand command)
{
    asio::post(m_context, [this, command = std::move(command)] {
        if (!m_outbound_queue.try_send(asio::error_code{}, std::move(command))) {
            abortGame("NetServer outbound queue full! Capacity = {}", OUTBOUND_QUEUE_MAX_SIZE);
        }
    });
}

asio::awaitable<void> NetServer::sendLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

    GC_ASSERT(m_socket);

    OutboundCommand command{};
    std::array<uint8_t, NET_MAX_PACKET_SIZE> buffer{};
    for (;;) {
        std::tie(ec, command) = co_await m_outbound_queue.async_receive(TOKEN);
        if (ec) {
            GC_ERROR("Outbound channel receive error: {}", ec.message());
            continue;
        }

        NetByteWriter writer(buffer);
        asio::ip::udp::endpoint endpoint{};

        std::visit(
            overloaded{
                [&](const OutboundCommand::OutboundConnectChallenge& challenge) {
                    endpoint = challenge.client_endpoint;
                    writePacketWithHeader(writer, challenge.session_token, NetPacketConnectChallenge{.client_nonce = challenge.client_nonce});
                },
                [&](const OutboundCommand::OutboundUnicast& unicast) {
                    auto it = m_sessions.find(unicast.session_token);
                    if (it != m_sessions.end()) {
                        NetSession& session = it->second;
                        endpoint = session.endpoint;
                        NetPacketMessage message{};
                        message.seq_num = session.next_seq_num;
                        message.ack_num = session.last_ack_num;
                        message.ack_bits = session.ack_bits;
                        message.payload_type = unicast.payload_type;
                        GC_ASSERT(unicast.payload.size() <= NET_MAX_PACKET_SIZE - NetPacketHeader::getSerialisedSize() - NetPacketMessage::getSerialisedSize());
                        message.payload_size = static_cast<uint16_t>(unicast.payload.size());
                        writePacketWithHeader(writer, session.session_token, message);
                        GC_ASSERT(writer.remaining() >= message.payload_size);
                        writer.writeBytes(unicast.payload);

                        session.next_seq_num += 1;
                    }
                },
                [&](const OutboundCommand::OutboundMulticast& multicast) { abortGame("haven't implemented this yet"); },
            },
            command.command);

        if (writer.pos() == 0) {
            continue;
        }

        size_t bytes_written{};
        std::tie(ec, bytes_written) = co_await m_socket->async_send_to(asio::buffer(buffer.data(), writer.pos()), endpoint, TOKEN);
        if (ec) {
            GC_ERROR("Failed to send from socket: {}", ec.message());
        }
        else if (bytes_written != writer.pos()) {
            GC_ERROR("Failed to send all data from socket. {}/{} bytes", bytes_written, writer.pos());
        }
    }
}

asio::awaitable<void> NetServer::receiveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

    GC_ASSERT(m_socket);

    const uint64_t server_secret = generateServerSecret();

    std::optional<uint32_t> last_cleanup_bucket{};
    std::array<uint8_t, NET_MAX_PACKET_SIZE> recv_buf{};
    std::array<uint8_t, NET_MAX_PACKET_SIZE> send_buf{};
    for (;;) {
        size_t bytes_read{};
        asio::ip::udp::endpoint client_endpoint{};
        std::tie(ec, bytes_read) = co_await m_socket->async_receive_from(asio::buffer(recv_buf), client_endpoint, TOKEN);
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
            handleAuthenticated(ctx, m_sessions);
        }

        // Put new packet on the outbound queue
        if (writer.pos() > 0) {
            OutboundPacket outbound{};
            outbound.endpoint = client_endpoint;
            outbound.data = std::vector<uint8_t>(send_buf.data(), send_buf.data() + writer.pos());
            std::tie(ec) = co_await m_outbound_queue.async_send(asio::error_code{}, std::move(outbound), TOKEN);
            if (ec) {
                GC_ERROR("Outbound channel send error: {}", ec.message());
                continue;
            }
        }
    }
}

// periodically checks all active sessions to see if they have timed out and removes them if so.
// Also pings clients that are currently idle (no data received recently).
asio::awaitable<void> NetServer::keepAliveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    asio::error_code ec{};

    constexpr auto TIME_PERIOD = std::chrono::seconds(1);
    constexpr int64_t TIMEOUT_TIME_NS = 5'000'000'000LL;
    constexpr int64_t IDLE_TIME_NS = 500'000'000LL;

    for (;;) {
        // maybe timer can be reset every iteration instead of reconstructing?
        asio::steady_timer timer(executor, TIME_PERIOD);

        const int64_t now = SDL_GetTicksNS();

        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            auto& session = it->second;
            const int64_t difference = now - static_cast<int64_t>(session.last_received_timestamp);
            if (difference > TIMEOUT_TIME_NS) {
                GC_INFO("Removing timed out session: {}", session.endpoint);
                it = m_sessions.erase(it);
            }
            else {
                if (difference > IDLE_TIME_NS) {
                    // send keepalive packet with no data
                    OutboundPacket packet{};
                    packet.endpoint = session.endpoint;
                    packet.data.resize(NetPacketHeader::getSerialisedSize() + NetPacketMessage::getSerialisedSize());
                    NetByteWriter writer(packet.data);

                    NetPacketMessage message{};
                    message.seq_num = session.next_seq_num;
                    message.ack_num = session.last_ack_num;
                    message.ack_bits = session.ack_bits;
                    message.payload_type = 0;
                    message.payload_size = 0;
                    writePacketWithHeader(writer, session.session_token, message);

                    session.next_seq_num += 1;

                    std::tie(ec) = co_await m_outbound_queue.async_send(asio::error_code{}, std::move(packet), TOKEN);
                    if (ec) {
                        GC_ERROR("Outbound channel send error: {}", ec.message());
                        continue;
                    }
                }
                ++it;
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
