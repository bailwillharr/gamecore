#include "gamecore/gc_net_client.h"

#include <array>

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
static constexpr auto HANDSHAKE_RESEND_PERIOD = std::chrono::milliseconds(250);
static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(5);
static constexpr int DISCONNECT_PACKET_COPIES = 2;

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    GC_ASSERT(!m_started);
    if (m_started) {
        return false;
    }

    // any local port
    if (!m_socket.open(asio::ip::udp::endpoint(endpoint.protocol(), 0))) {
        return false;
    }
    m_started = true;
    m_server_endpoint = endpoint;
    GC_INFO("Connecting to server: {}", endpoint);

    m_socket.start([this](std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender) { onPacket(packet, sender); },
                   [this] { close(NetDisconnectReason::SOCKET_FAILED, false); });
    asio::co_spawn(m_context, tickLoop(), asio::detached);
    asio::post(m_context, [this] {
        m_client_nonce = generateNetRandom64();
        m_connect_start_time = NetClock::now();
        sendConnectRequest();
    });

    // Set before the thread starts so that the status is never DISCONNECTED while the connection is in progress
    m_status.store(NetClientConnectionStatus::CONNECTING);
    m_client_thread = std::jthread([this] {
        m_context.run(); // returns when close() stops the context
        m_status.store(NetClientConnectionStatus::DISCONNECTED);
    });

    return true;
}

void NetClient::disconnect()
{
    if (m_client_thread.joinable()) {
        // If the context has already stopped then the connection is already closed and this never runs
        asio::post(m_context, [this] { close(NetDisconnectReason::LOCAL_REQUEST, true); });
        m_client_thread.join();
    }
    m_socket.close();
    GC_ASSERT(getConnectionStatus() == NetClientConnectionStatus::DISCONNECTED);
}

bool NetClient::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

NetClientConnectionStatus NetClient::getConnectionStatus() const { return m_status.load(); }

asio::ip::udp::endpoint NetClient::getServerEndpoint() const { return m_server_endpoint; }

NetPeerId NetClient::getLocalPeerId() const { return m_local_peer_id.load(); }

NetConnectionStats NetClient::getStats() const
{
    std::scoped_lock lock(m_shared.mutex);
    return m_shared.stats;
}

void NetClient::sendMessage(std::vector<uint8_t> message, NetDelivery delivery)
{
    asio::post(m_context, [this, message = std::move(message), delivery] {
        if (m_phase != Phase::CONNECTED) {
            return;
        }
        if (!m_connection->queueMessage(delivery, message)) {
            GC_WARN("Server isn't acknowledging reliable messages fast enough");
            close(NetDisconnectReason::SEND_QUEUE_OVERFLOW, true);
            return;
        }
        scheduleFlush();
    });
}

void NetClient::setSimConfig(const NetSimConfig& config) { m_socket.setSimConfig(config); }

asio::awaitable<void> NetClient::tickLoop()
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

void NetClient::onPacket(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender)
{
    if (sender != m_server_endpoint || m_phase == Phase::CLOSED) {
        return;
    }

    ByteReader reader(packet);
    const auto header = tryDeserialise<NetPacketHeader>(reader);
    if (!header || !verifyPacketHeader(*header)) {
        return;
    }

    switch (header->type) {
    case NetPacketType::CONNECT_CHALLENGE: {
        if (m_phase != Phase::REQUEST) {
            break;
        }
        const auto challenge = tryDeserialiseExact<NetPacketConnectChallenge>(reader);
        if (challenge && challenge->client_nonce == m_client_nonce) {
            m_session_token = header->token;
            m_phase = Phase::RESPONSE;
            sendConnectChallengeResponse();
        }
    } break;
    case NetPacketType::CONNECT_ACCEPT: {
        if (m_phase != Phase::RESPONSE || header->token != m_session_token) {
            break;
        }
        const auto accept = tryDeserialiseExact<NetPacketConnectAccept>(reader);
        if (accept && accept->client_nonce == m_client_nonce) {
            m_connection.emplace(NetClock::now());
            m_phase = Phase::CONNECTED;
            m_local_peer_id.store(accept->peer_id);
            m_status.store(NetClientConnectionStatus::CONNECTED);
            GC_INFO("Connected to server as client {}", accept->peer_id);

            NetEvent ev{};
            ev.kind = NetEventKind::CONNECTED;
            ev.peer = NET_PEER_SERVER;
            m_event_queue.push(std::move(ev));
        }
    } break;
    case NetPacketType::DATA: {
        if (m_phase != Phase::CONNECTED || header->token != m_session_token) {
            break;
        }
        const auto now = NetClock::now();
        m_received_messages.clear();
        m_connection->receivePacket(reader, now, m_received_messages);
        for (auto& message : m_received_messages) {
            if (auto ev = decodeNetEvent(std::move(message), NET_PEER_SERVER)) {
                m_event_queue.push(std::move(*ev));
            }
        }
        // sends an acknowledgement straight away if the packet needs one
        flush(now);
    } break;
    case NetPacketType::DISCONNECT: {
        if (m_phase == Phase::REQUEST || header->token != m_session_token) {
            break;
        }
        const auto disconnect_packet = tryDeserialiseExact<NetPacketDisconnect>(reader);
        if (!disconnect_packet) {
            break;
        }
        switch (disconnect_packet->reason) {
        case NetDisconnectReason::TIMED_OUT:
        case NetDisconnectReason::KICKED:
        case NetDisconnectReason::SERVER_FULL:
        case NetDisconnectReason::SERVER_SHUTDOWN:
        case NetDisconnectReason::SEND_QUEUE_OVERFLOW:
            close(disconnect_packet->reason, false);
            break;
        default:
            close(NetDisconnectReason::REMOTE_CLOSED, false);
            break;
        }
    } break;
    default:
        // the remaining packet types are only sent by clients
        break;
    }
}

void NetClient::sendConnectRequest()
{
    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketConnectRequest::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, NetSessionToken{0}, NetPacketConnectRequest{.client_nonce = m_client_nonce});
    m_socket.send(buffer, m_server_endpoint);
    m_last_handshake_send_time = NetClock::now();
}

void NetClient::sendConnectChallengeResponse()
{
    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketConnectChallengeResponse::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, m_session_token, NetPacketConnectChallengeResponse{.client_nonce = m_client_nonce});
    m_socket.send(buffer, m_server_endpoint);
    m_last_handshake_send_time = NetClock::now();
}

// Only used when the client is about to stop, so this can't be delayed by the link simulator
void NetClient::sendDisconnect(NetDisconnectReason reason)
{
    std::array<uint8_t, NetPacketHeader::getSerialisedSize() + NetPacketDisconnect::getSerialisedSize()> buffer{};
    ByteWriter writer(buffer);
    writePacketWithHeader(writer, m_session_token, NetPacketDisconnect{.reason = reason});
    for (int i = 0; i < DISCONNECT_PACKET_COPIES; ++i) {
        m_socket.sendDirect(buffer, m_server_endpoint);
    }
}

void NetClient::flush(NetClock::time_point now)
{
    std::array<uint8_t, NET_MAX_PACKET_SIZE> buffer{};
    for (;;) {
        ByteWriter writer(buffer);
        NetPacketHeader::createValid(NetPacketType::DATA, m_session_token).serialise(writer);
        if (!m_connection->writePacket(writer, now)) {
            break;
        }
        m_socket.send(std::span<const uint8_t>(buffer.data(), writer.pos()), m_server_endpoint);
    }
}

// Messages posted from the main thread during the same frame are all queued before this runs, so they share packets.
void NetClient::scheduleFlush()
{
    if (m_flush_scheduled) {
        return;
    }
    m_flush_scheduled = true;
    asio::post(m_context, [this] {
        m_flush_scheduled = false;
        if (m_phase == Phase::CONNECTED) {
            flush(NetClock::now());
        }
    });
}

// Repeats handshake packets until the server answers. Once connected, checks if the server has timed out, retransmits reliable
// messages that haven't been acknowledged, sends delayed acknowledgements, and pings the server if the connection is idle.
void NetClient::tick()
{
    const auto now = NetClock::now();

    switch (m_phase) {
    case Phase::REQUEST:
    case Phase::RESPONSE:
        if (now - m_connect_start_time > CONNECT_TIMEOUT) {
            close(NetDisconnectReason::CONNECT_FAILED, false);
        }
        else if (now - m_last_handshake_send_time >= HANDSHAKE_RESEND_PERIOD) {
            if (m_phase == Phase::REQUEST) {
                sendConnectRequest();
            }
            else {
                sendConnectChallengeResponse();
            }
        }
        break;
    case Phase::CONNECTED:
        if (m_connection->hasTimedOut(now)) {
            close(NetDisconnectReason::TIMED_OUT, true);
        }
        else {
            flush(now);
            m_connection->updateStats(now);
            std::scoped_lock lock(m_shared.mutex);
            m_shared.stats = m_connection->getStats();
        }
        break;
    case Phase::CLOSED:
        break;
    }
}

void NetClient::close(NetDisconnectReason reason, bool notify_server)
{
    if (m_phase == Phase::CLOSED) {
        return;
    }
    GC_INFO("Disconnected from server: {}", netDisconnectReasonString(reason));

    // The server might have created the session even if its accept packet never arrived
    if (notify_server && m_session_token != 0) {
        sendDisconnect(reason);
    }

    NetEvent ev{};
    ev.kind = NetEventKind::DISCONNECTED;
    ev.peer = NET_PEER_SERVER;
    ev.reason = reason;
    m_event_queue.push(std::move(ev));

    m_phase = Phase::CLOSED;
    m_context.stop(); // Stopping the context is safe to do inside a completion handler.
}

} // namespace gc
