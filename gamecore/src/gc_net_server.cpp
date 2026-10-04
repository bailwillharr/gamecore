#include "gamecore/gc_net_server.h"

#include <cstring>

#include <algorithm>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <gclog/gclog.h>

#include "gamecore/gc_assert.h"

namespace gc {

static constexpr auto TICK_PERIOD = std::chrono::milliseconds(10);
static constexpr auto PUBLISH_PERIOD = std::chrono::milliseconds(100);
static constexpr auto TIME_BUCKET_LENGTH = std::chrono::seconds(10);
static constexpr auto CLOSED_TOKEN_LIFETIME = 3 * TIME_BUCKET_LENGTH; // longer than a token can pass verification for
static constexpr int DISCONNECT_PACKET_COPIES = 2;

static uint32_t getTimeBucket(NetClock::time_point now)
{
    return static_cast<uint32_t>(now.time_since_epoch() / TIME_BUCKET_LENGTH); // truncates to uint32_t
}

static uint64_t rotateLeft(uint64_t x, int bits) { return (x << bits) | (x >> (64 - bits)); }

// SipHash-2-4. A keyed hash designed for authenticating short messages.
// https://www.aumasson.jp/siphash/siphash.pdf
static uint64_t sipHash(const std::array<uint64_t, 2>& key, std::span<const uint8_t> data)
{
    uint64_t v0 = 0x736f6d6570736575ULL ^ key[0];
    uint64_t v1 = 0x646f72616e646f6dULL ^ key[1];
    uint64_t v2 = 0x6c7967656e657261ULL ^ key[0];
    uint64_t v3 = 0x7465646279746573ULL ^ key[1];

    const auto sip_round = [&] {
        v0 += v1;
        v1 = rotateLeft(v1, 13);
        v1 ^= v0;
        v0 = rotateLeft(v0, 32);
        v2 += v3;
        v3 = rotateLeft(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotateLeft(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotateLeft(v1, 17);
        v1 ^= v2;
        v2 = rotateLeft(v2, 32);
    };

    size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        uint64_t m{};
        std::memcpy(&m, data.data() + i, sizeof(m));
        v3 ^= m;
        sip_round();
        sip_round();
        v0 ^= m;
    }

    uint64_t last = static_cast<uint64_t>(data.size()) << 56;
    for (size_t j = 0; i + j < data.size(); ++j) {
        last |= static_cast<uint64_t>(data[i + j]) << (8 * j);
    }
    v3 ^= last;
    sip_round();
    sip_round();
    v0 ^= last;

    v2 ^= 0xff;
    sip_round();
    sip_round();
    sip_round();
    sip_round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// The token is derived from things only the server and the real owner of the client's address know, so the server doesn't need to
// remember anything about a client until it proves it can receive packets at the address it claims to have.
// Packets aren't encrypted, so this doesn't protect against an attacker that can read the traffic between client and server.
static NetSessionToken computeSessionToken(const std::array<uint64_t, 2>& server_secret, const asio::ip::udp::endpoint& client_endpoint, uint64_t client_nonce,
                                           uint32_t time_bucket)
{
    struct Data {
        std::array<uint8_t, 16> address;
        uint64_t client_nonce;
        uint32_t time_bucket;
        uint16_t port;
        uint16_t padding; // must be zero
    };
    static_assert(sizeof(Data) == 32);
    static_assert(std::endian::native == std::endian::little);
    static_assert(std::is_same_v<asio::ip::port_type, uint16_t>);

    Data data{};
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

    const uint64_t hash = sipHash(server_secret, std::span(reinterpret_cast<const uint8_t*>(&data), sizeof(data)));

    return NetSessionToken{hash != 0 ? hash : 1}; // zero means 'no session'
}

NetServer::Session::Session(NetPeerId id, NetSessionToken session_token, const asio::ip::udp::endpoint& client_endpoint, NetClock::time_point now)
    : peer_id(id), token(session_token), endpoint(client_endpoint), connection(now)
{
}

NetServer::~NetServer() { stop(); }

bool NetServer::start(const asio::ip::udp::endpoint& endpoint)
{
    GC_ASSERT(!m_started);
    if (m_started) {
        return false;
    }

    if (!m_socket.open(endpoint)) {
        return false;
    }
    m_started = true;

    {
        std::scoped_lock lock(m_shared.mutex);
        m_shared.local_endpoint = m_socket.getLocalEndpoint();
    }
    GC_INFO("Starting server on {}", m_socket.getLocalEndpoint());

    m_secret = {generateNetRandom64(), generateNetRandom64()};

    m_socket.start([this](std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender) { onPacket(packet, sender); },
                   [this] {
                       GC_ERROR("Server socket failed, stopping server");
                       closeAllSessions(NetDisconnectReason::SOCKET_FAILED);
                       m_context.stop();
                   });
    asio::co_spawn(m_context, tickLoop(), asio::detached);

    // Set before the thread starts so that isRunning() is true as soon as start() returns
    m_running.store(true);
    m_server_thread = std::jthread([this] {
        m_context.run();
        m_running.store(false);
    });

    return true;
}

void NetServer::stop()
{
    if (m_server_thread.joinable()) {
        // If the context has already stopped this never runs, which is fine as there is nobody left to tell.
        asio::post(m_context, [this] {
            closeAllSessions(NetDisconnectReason::SERVER_SHUTDOWN);
            m_context.stop(); // Stopping the context is safe to do inside a completion handler.
        });
        m_server_thread.join();
        GC_INFO("Server stopped");
    }
    m_socket.close();
}

bool NetServer::isRunning() const { return m_running.load(); }

asio::ip::udp::endpoint NetServer::getLocalEndpoint() const
{
    std::scoped_lock lock(m_shared.mutex);
    return m_shared.local_endpoint;
}

uint32_t NetServer::getPeerCount() const
{
    std::scoped_lock lock(m_shared.mutex);
    return static_cast<uint32_t>(m_shared.peers.size());
}

std::vector<NetPeerInfo> NetServer::getPeers() const
{
    std::scoped_lock lock(m_shared.mutex);
    return m_shared.peers;
}

bool NetServer::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

void NetServer::sendMessage(std::vector<uint8_t> message, NetDelivery delivery, NetPeerId peer)
{
    asio::post(m_context, [this, message = std::move(message), delivery, peer] {
        std::vector<NetSessionToken> overflowed{};
        if (peer != NET_PEER_NONE) {
            if (const auto token_it = m_peer_tokens.find(peer); token_it != m_peer_tokens.end()) {
                queueMessage(m_sessions.at(token_it->second), delivery, message, overflowed);
            }
        }
        else {
            for (auto& [token, session] : m_sessions) {
                queueMessage(session, delivery, message, overflowed);
            }
        }
        // removed afterwards so that m_sessions isn't modified while iterating it
        for (const NetSessionToken token : overflowed) {
            removeSession(m_sessions.find(token), NetDisconnectReason::SEND_QUEUE_OVERFLOW, true);
        }
        scheduleFlush();
    });
}

void NetServer::disconnectPeer(NetPeerId peer)
{
    asio::post(m_context, [this, peer] {
        if (const auto token_it = m_peer_tokens.find(peer); token_it != m_peer_tokens.end()) {
            removeSession(m_sessions.find(token_it->second), NetDisconnectReason::KICKED, true);
        }
    });
}

void NetServer::setSimConfig(const NetSimConfig& config) { m_socket.setSimConfig(config); }

asio::awaitable<void> NetServer::tickLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    asio::steady_timer timer(co_await asio::this_coro::executor);
    for (;;) {
        timer.expires_after(TICK_PERIOD);
        const auto [ec] = co_await timer.async_wait(TOKEN);
        if (ec) {
            co_return;
        }
        tick();
    }
}

void NetServer::onPacket(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender)
{
    ByteReader reader(packet);
    const auto header = tryDeserialise<NetPacketHeader>(reader);
    if (!header || !verifyPacketHeader(*header)) {
        return;
    }

    switch (header->type) {
    case NetPacketType::CONNECT_REQUEST:
        onConnectRequest(reader, sender);
        break;
    case NetPacketType::CONNECT_CHALLENGE_RESPONSE:
        onConnectChallengeResponse(reader, header->token, sender);
        break;
    case NetPacketType::DATA:
        if (Session* session = findSession(header->token, sender)) {
            onData(reader, *session);
        }
        break;
    case NetPacketType::DISCONNECT:
        if (findSession(header->token, sender) && tryDeserialiseExact<NetPacketDisconnect>(reader)) {
            removeSession(m_sessions.find(header->token), NetDisconnectReason::REMOTE_CLOSED, false);
        }
        break;
    default:
        // the remaining packet types are only sent by servers
        break;
    }
}

void NetServer::onConnectRequest(ByteReader& reader, const asio::ip::udp::endpoint& sender)
{
    // Requests are handled statelessly. Nothing is stored until the client echoes the token back from the same address.
    const auto request = tryDeserialiseExact<NetPacketConnectRequest>(reader);
    if (!request) {
        return;
    }
    const auto token = computeSessionToken(m_secret, sender, request->client_nonce, getTimeBucket(NetClock::now()));

    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketConnectChallenge::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, token, NetPacketConnectChallenge{.client_nonce = request->client_nonce});
    m_socket.send(buffer, sender);
}

void NetServer::onConnectChallengeResponse(ByteReader& reader, NetSessionToken token, const asio::ip::udp::endpoint& sender)
{
    const auto response = tryDeserialiseExact<NetPacketConnectChallengeResponse>(reader);
    if (!response) {
        return;
    }

    if (const Session* session = findSession(token, sender)) {
        // The session already exists, so the client must have missed the accept packet
        sendConnectAccept(*session, response->client_nonce);
        return;
    }
    if (m_sessions.contains(token)) {
        return; // another address is trying to use this session's token
    }

    const auto now = NetClock::now();

    // Also compare against what the session token would have been during the previous time bucket.
    const uint32_t time_bucket = getTimeBucket(now);
    if (token != computeSessionToken(m_secret, sender, response->client_nonce, time_bucket) &&
        token != computeSessionToken(m_secret, sender, response->client_nonce, time_bucket - 1)) {
        return;
    }

    if (m_closed_tokens.contains(token)) {
        return; // a late copy of a response for a session that has since ended
    }

    if (m_sessions.size() >= MAX_CLIENTS) {
        GC_INFO("Rejecting connection from {}: server full", sender);
        sendDisconnect(token, sender, NetDisconnectReason::SERVER_FULL, false);
        return;
    }

    const NetPeerId peer_id = m_next_peer_id++;
    const auto [it, inserted] = m_sessions.try_emplace(token, peer_id, token, sender, now);
    GC_ASSERT(inserted);
    m_peer_tokens.emplace(peer_id, token);
    GC_INFO("Client {} connected from {}", peer_id, sender);

    sendConnectAccept(it->second, response->client_nonce);

    NetEvent ev{};
    ev.kind = NetEventKind::CONNECTED;
    ev.peer = peer_id;
    m_event_queue.push(std::move(ev));

    publishPeers();
}

void NetServer::onData(ByteReader& reader, Session& session)
{
    const auto now = NetClock::now();

    m_received_messages.clear();
    session.connection.receivePacket(reader, now, m_received_messages);
    for (auto& message : m_received_messages) {
        if (auto ev = decodeNetEvent(std::move(message), session.peer_id)) {
            m_event_queue.push(std::move(*ev));
        }
    }

    // sends an acknowledgement straight away if the packet needs one
    flushSession(session, now);
}

NetServer::Session* NetServer::findSession(NetSessionToken token, const asio::ip::udp::endpoint& sender)
{
    const auto it = m_sessions.find(token);
    if (it == m_sessions.end()) {
        return nullptr;
    }
    if (it->second.endpoint != sender) {
        // Might just be a NAT rebind or carrier handoff.
        // TODO: Handle endpoint migration
        GC_DEBUG("Packet from {} has session token corresponding to existing session with {}", sender, it->second.endpoint);
        return nullptr;
    }
    return &it->second;
}

void NetServer::removeSession(SessionMap::iterator it, NetDisconnectReason reason, bool notify_client)
{
    if (it == m_sessions.end()) {
        return;
    }
    const Session& session = it->second;
    GC_INFO("Client {} ({}) disconnected: {}", session.peer_id, session.endpoint, netDisconnectReasonString(reason));

    if (notify_client) {
        sendDisconnect(session.token, session.endpoint, reason, false);
    }

    NetEvent ev{};
    ev.kind = NetEventKind::DISCONNECTED;
    ev.peer = session.peer_id;
    ev.reason = reason;
    m_event_queue.push(std::move(ev));

    m_closed_tokens.emplace(session.token, NetClock::now() + CLOSED_TOKEN_LIFETIME);
    m_peer_tokens.erase(session.peer_id);
    m_sessions.erase(it);

    publishPeers();
}

void NetServer::queueMessage(Session& session, NetDelivery delivery, std::span<const uint8_t> message, std::vector<NetSessionToken>& overflowed)
{
    if (!session.connection.queueMessage(delivery, message)) {
        GC_WARN("Client {} isn't acknowledging reliable messages fast enough", session.peer_id);
        overflowed.push_back(session.token);
    }
}

void NetServer::flushSession(Session& session, NetClock::time_point now)
{
    std::array<uint8_t, NET_MAX_PACKET_SIZE> buffer{};
    for (;;) {
        ByteWriter writer(buffer);
        NetPacketHeader::createValid(NetPacketType::DATA, session.token).serialise(writer);
        if (!session.connection.writePacket(writer, now)) {
            break;
        }
        m_socket.send(std::span<const uint8_t>(buffer.data(), writer.pos()), session.endpoint);
    }
}

// Messages posted from the main thread during the same frame are all queued before this runs, so they share packets.
void NetServer::scheduleFlush()
{
    if (m_flush_scheduled) {
        return;
    }
    m_flush_scheduled = true;
    asio::post(m_context, [this] {
        m_flush_scheduled = false;
        const auto now = NetClock::now();
        for (auto& [token, session] : m_sessions) {
            flushSession(session, now);
        }
    });
}

void NetServer::sendConnectAccept(const Session& session, uint64_t client_nonce)
{
    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketConnectAccept::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, session.token, NetPacketConnectAccept{.client_nonce = client_nonce, .peer_id = session.peer_id});
    m_socket.send(buffer, session.endpoint);
}

void NetServer::sendDisconnect(NetSessionToken token, const asio::ip::udp::endpoint& endpoint, NetDisconnectReason reason, bool bypass_simulator)
{
    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketDisconnect::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, token, NetPacketDisconnect{.reason = reason});
    for (int i = 0; i < DISCONNECT_PACKET_COPIES; ++i) {
        if (bypass_simulator) {
            m_socket.sendDirect(buffer, endpoint);
        }
        else {
            m_socket.send(buffer, endpoint);
        }
    }
}

// Removes sessions that have timed out, retransmits reliable messages that haven't been acknowledged, sends delayed
// acknowledgements, and pings clients that are idle so that both sides can tell that the connection is still alive.
void NetServer::tick()
{
    const auto now = NetClock::now();

    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        const auto current = it++; // removing the session only invalidates the iterator to that session
        Session& session = current->second;
        if (session.connection.hasTimedOut(now)) {
            removeSession(current, NetDisconnectReason::TIMED_OUT, true);
        }
        else {
            flushSession(session, now);
            session.connection.updateStats(now);
        }
    }

    std::erase_if(m_closed_tokens, [now](const auto& closed_token) { return closed_token.second <= now; });

    if (now - m_last_publish_time >= PUBLISH_PERIOD) {
        publishPeers();
    }
}

void NetServer::publishPeers()
{
    std::scoped_lock lock(m_shared.mutex);
    m_shared.peers.clear();
    for (const auto& [token, session] : m_sessions) {
        m_shared.peers.push_back(NetPeerInfo{.id = session.peer_id, .endpoint = session.endpoint, .stats = session.connection.getStats()});
    }
    std::sort(m_shared.peers.begin(), m_shared.peers.end(), [](const NetPeerInfo& a, const NetPeerInfo& b) { return a.id < b.id; });
    m_last_publish_time = NetClock::now();
}

// Used when the server is about to stop, so nothing sent here can be delayed
void NetServer::closeAllSessions(NetDisconnectReason reason)
{
    for (const auto& [token, session] : m_sessions) {
        sendDisconnect(token, session.endpoint, reason, true);

        NetEvent ev{};
        ev.kind = NetEventKind::DISCONNECTED;
        ev.peer = session.peer_id;
        ev.reason = reason;
        m_event_queue.push(std::move(ev));
    }
    m_sessions.clear();
    m_peer_tokens.clear();
    publishPeers();
}

} // namespace gc
