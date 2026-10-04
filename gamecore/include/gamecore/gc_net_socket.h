#pragma once

// A UDP socket with an optional link simulator in front of it.
// When the simulator is enabled, packets in both directions can be dropped, duplicated and delayed, which makes it possible to
// test how the transport copes with a bad connection without any external tools.

#include <cstdint>

#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <span>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "gamecore/gc_net_common.h"

namespace gc {

class NetSocket {
public:
    using PacketHandler = std::function<void(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& sender)>;
    using ErrorHandler = std::function<void()>;

private:
    struct DelayedPacket {
        asio::ip::udp::endpoint endpoint{};
        std::vector<uint8_t> data{};
        bool outgoing{};
    };

    asio::ip::udp::socket m_socket;
    asio::steady_timer m_delay_timer;
    PacketHandler m_packet_handler{};
    ErrorHandler m_error_handler{};
    std::multimap<NetClock::time_point, DelayedPacket> m_delayed_packets{}; // ordered by release time
    std::mt19937 m_rng;

    mutable std::mutex m_sim_config_mutex{};
    NetSimConfig m_sim_config{};

public:
    explicit NetSocket(asio::io_context& context);

    NetSocket(const NetSocket&) = delete;

    NetSocket& operator=(const NetSocket&) = delete;

    // Returns false on failure. An IPv6 socket bound to the unspecified address (::) also accepts IPv4.
    bool open(const asio::ip::udp::endpoint& bind_endpoint);
    void close();
    asio::ip::udp::endpoint getLocalEndpoint() const;

    // Starts receiving packets. The handlers are called from whichever thread runs the io_context.
    // error_handler is called if the socket fails and can no longer receive.
    void start(PacketHandler packet_handler, ErrorHandler error_handler);

    // These can be called from any thread:
    void setSimConfig(const NetSimConfig& config);
    NetSimConfig getSimConfig() const;

    // The remaining functions must only be called from the thread running the io_context.

    // Sends a packet through the link simulator
    void send(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint);

    // Sends a packet immediately, bypassing the link simulator
    void sendDirect(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint);

private:
    asio::awaitable<void> receiveLoop();

    // returns true if the simulator took the packet (it was dropped or will be delivered later)
    bool simulate(std::span<const uint8_t> packet, const asio::ip::udp::endpoint& endpoint, bool outgoing);
    void armDelayTimer();
    void releaseDelayedPackets();
};

} // namespace gc
