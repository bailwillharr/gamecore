#include "gamecore/gc_net_socket.h"

#include <algorithm>
#include <array>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/socket_base.hpp>
#include <asio/use_awaitable.hpp>

#include <gclog/gclog.h>

#include "gamecore/gc_assert.h"

namespace gc {

NetSocket::NetSocket(asio::io_context& context) : m_socket(context), m_delay_timer(context), m_rng(std::random_device{}()) {}

bool NetSocket::open(const asio::ip::udp::endpoint& bind_endpoint)
{
    asio::error_code ec{};
    m_socket.open(bind_endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        return false;
    }
    if (bind_endpoint.protocol() == asio::ip::udp::v6()) {
        // Windows defaults to IPv6 only. Not all platforms allow this to be changed so failure is fine.
        m_socket.set_option(asio::ip::v6_only(false), ec);
        ec.clear();
    }
    // A server receives from all of its clients on this one socket, so don't let a burst overflow the default buffer
    m_socket.set_option(asio::socket_base::receive_buffer_size(1024 * 1024), ec);
    ec.clear();
    m_socket.bind(bind_endpoint, ec);
    if (ec) {
        GC_ERROR("Failed to bind socket to {}: {}", bind_endpoint, ec.message());
        m_socket.close(ec);
        return false;
    }
    return true;
}

void NetSocket::close()
{
    asio::error_code ec{};
    m_delay_timer.cancel();
    m_socket.close(ec);
    m_delayed_packets.clear();
}

asio::ip::udp::endpoint NetSocket::getLocalEndpoint() const
{
    asio::error_code ec{};
    return m_socket.local_endpoint(ec);
}

void NetSocket::start(PacketHandler packet_handler, ErrorHandler error_handler)
{
    GC_ASSERT(m_socket.is_open());
    m_packet_handler = std::move(packet_handler);
    m_error_handler = std::move(error_handler);
    asio::co_spawn(m_socket.get_executor(), receiveLoop(), asio::detached);
}

void NetSocket::setSimConfig(const NetSimConfig& config)
{
    std::scoped_lock lock(m_sim_config_mutex);
    m_sim_config = config;
}

NetSimConfig NetSocket::getSimConfig() const
{
    std::scoped_lock lock(m_sim_config_mutex);
    return m_sim_config;
}

void NetSocket::send(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint)
{
    if (!simulate(packet, endpoint, true)) {
        sendDirect(packet, endpoint);
    }
}

void NetSocket::sendDirect(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint)
{
    // UDP sends don't wait for the network so there is nothing to gain from making this asynchronous
    asio::error_code ec{};
    m_socket.send_to(asio::buffer(packet.data(), packet.size()), endpoint, 0, ec);
    if (ec && ec != asio::error::would_block && ec != asio::error::connection_refused && ec != asio::error::connection_reset) {
        GC_ERROR("Failed to send {} bytes to {}: {}", packet.size(), endpoint, ec.message());
    }
}

asio::awaitable<void> NetSocket::receiveLoop()
{
    constexpr auto TOKEN = asio::as_tuple(asio::use_awaitable);
    constexpr int MAX_CONSECUTIVE_ERRORS = 32;

    int consecutive_errors = 0;
    std::array<uint8_t, NET_MAX_PACKET_SIZE> buffer{};
    asio::ip::udp::endpoint sender{};
    for (;;) {
        const auto [ec, size] = co_await m_socket.async_receive_from(asio::buffer(buffer), sender, TOKEN);
        if (ec == asio::error::operation_aborted || !m_socket.is_open()) {
            co_return; // socket was closed
        }
        if (ec == asio::error::connection_refused || ec == asio::error::connection_reset || ec == asio::error::message_size) {
            // The first two are reported when an earlier send reached a host that isn't listening (any more).
            // The remote host timing out takes care of that. The last is an oversized datagram that can't be one of ours.
            continue;
        }
        if (ec) {
            GC_ERROR("Failed to receive on socket: {}", ec.message());
            if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                if (m_error_handler) {
                    m_error_handler();
                }
                co_return;
            }
            continue;
        }
        consecutive_errors = 0;

        const std::span<const uint8_t> packet(buffer.data(), size);
        if (!simulate(packet, sender, false)) {
            m_packet_handler(packet, sender);
        }
    }
}

bool NetSocket::simulate(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint, bool outgoing)
{
    const NetSimConfig config = getSimConfig();
    if (!config.enabled) {
        return false;
    }

    std::uniform_real_distribution<float> percent(0.0f, 100.0f);
    if (percent(m_rng) < config.loss_percent) {
        return true; // dropped
    }

    const int copies = (percent(m_rng) < config.duplicate_percent) ? 2 : 1;
    const auto now = NetClock::now();
    for (int i = 0; i < copies; ++i) {
        float delay_ms = config.latency_ms;
        if (config.jitter_ms > 0.0f) {
            delay_ms += std::uniform_real_distribution<float>(-config.jitter_ms, config.jitter_ms)(m_rng);
        }
        delay_ms = std::max(delay_ms, 0.0f);
        const auto release_time = now + std::chrono::duration_cast<NetClock::duration>(std::chrono::duration<float, std::milli>(delay_ms));

        DelayedPacket delayed{};
        delayed.endpoint = endpoint;
        delayed.data.assign(packet.begin(), packet.end());
        delayed.outgoing = outgoing;
        m_delayed_packets.emplace(release_time, std::move(delayed));
    }
    armDelayTimer();
    return true;
}

void NetSocket::armDelayTimer()
{
    if (m_delayed_packets.empty()) {
        return;
    }
    // Setting the expiry cancels the wait that is already in progress, if there is one
    m_delay_timer.expires_at(m_delayed_packets.begin()->first);
    m_delay_timer.async_wait([this](const asio::error_code& ec) {
        if (!ec) {
            releaseDelayedPackets();
        }
    });
}

void NetSocket::releaseDelayedPackets()
{
    const auto now = NetClock::now();
    while (!m_delayed_packets.empty() && m_delayed_packets.begin()->first <= now) {
        // The packet is removed first as the handler can end up adding more delayed packets
        const auto node = m_delayed_packets.extract(m_delayed_packets.begin());
        const DelayedPacket& delayed = node.mapped();
        if (!m_socket.is_open()) {
            continue;
        }
        if (delayed.outgoing) {
            sendDirect(delayed.data, delayed.endpoint);
        }
        else {
            m_packet_handler(delayed.data, delayed.endpoint);
        }
    }
    armDelayTimer();
}

} // namespace gc
