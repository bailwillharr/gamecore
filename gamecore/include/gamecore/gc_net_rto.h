#pragma once

#include <algorithm>
#include <chrono>

namespace gc {

// based on https://www.rfc-editor.org/rfc/rfc6298
class RetransmitTimeoutCalculator {
public:
    using timer_duration = std::chrono::steady_clock::duration;

private:
    using calc_duration = std::chrono::duration<double>;

    static constexpr calc_duration RTO_INITIAL = std::chrono::milliseconds(250);
    static constexpr calc_duration RTO_MIN = std::chrono::milliseconds(80);
    static constexpr calc_duration RTO_MAX = std::chrono::milliseconds(2000);

    static constexpr double K = 4.0;
    static constexpr double ALPHA = 1.0 / 8.0;
    static constexpr double BETA = 1.0 / 4.0;

    bool m_first_rtt_recorded = false;
    calc_duration m_srtt{};
    calc_duration m_rttvar{};
    calc_duration m_rto{RTO_INITIAL};

public:
    timer_duration getRTO() const { return std::chrono::duration_cast<timer_duration>(m_rto); }

    int64_t getRTONanoseconds() const { return std::chrono::duration_cast<std::chrono::nanoseconds>(m_rto).count(); }

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

} // namespace gc