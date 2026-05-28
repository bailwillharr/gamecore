#include "gamecore/gc_net_client.h"

#include <random>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <chrono>

#include "gamecore/gc_net_common.h"

namespace gc {

[[maybe_unused]] static uint32_t generateClientNonce()
{
    std::mt19937 rand32(std::random_device{}());
    return rand32();
}

// based on https://www.rfc-editor.org/rfc/rfc6298
class RetransmitTimeoutCalculator {
public:
    using timer_duration = std::chrono::steady_clock::duration;

private:
    using calc_duration = std::chrono::duration<double>;

    static constexpr calc_duration RTO_INITIAL = std::chrono::seconds(1);
    static constexpr calc_duration RTO_MIN = std::chrono::seconds(1);
    static constexpr calc_duration RTO_MAX = std::chrono::seconds(60);

    static constexpr double K = 4.0;
    static constexpr double ALPHA = 1.0 / 8.0;
    static constexpr double BETA = 1.0 / 4.0;

    bool m_first_rtt_recorded = false;
    calc_duration m_srtt{};
    calc_duration m_rttvar{};
    calc_duration m_rto{RTO_INITIAL};

public:
    timer_duration getRTO() const { return std::chrono::duration_cast<timer_duration>(m_rto); }

    void recordRTT(timer_duration rtt)
    {
        calc_duration rtt_d = std::chrono::duration_cast<calc_duration>(rtt);

        if (!m_first_rtt_recorded) {
            m_srtt = rtt_d;
            m_rttvar = rtt_d / 2.0;
            m_first_rtt_recorded = true;
        }
        else {
            auto err = m_srtt - rtt_d;
            if (err < calc_duration::zero()) {
                err = -err;
            }

            m_rttvar = (1.0 - BETA) * m_rttvar + BETA * err;
            m_srtt = (1.0 - ALPHA) * m_srtt + ALPHA * rtt_d;
        }

        m_rto = m_srtt + K * m_rttvar;

        m_rto = std::clamp(m_rto, RTO_MIN, RTO_MAX);
    }
};

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    disconnect(); // just in case
    m_context.restart();
    m_state.store(NetClientState::CONNECTING);
    m_client_thread = std::jthread(
        [](NetClient& self, asio::ip::udp::endpoint endpoint) {
            //asio::co_spawn(self.m_context, self.clientLoop(endpoint), asio::detached);
            (void)endpoint;
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

} // namespace gc
