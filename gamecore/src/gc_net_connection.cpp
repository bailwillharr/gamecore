#include "gamecore/gc_net_connection.h"

#include <algorithm>

#include <gclog/gclog.h>

#include "gamecore/gc_assert.h"

namespace gc {

// After NetPacketData, a DATA packet contains zero or more messages laid out as:
//   uint8_t flags
//   uint16_t fragment_id   (only if NET_MESSAGE_FLAG_RELIABLE)
//   uint16_t size          (not present if NET_MESSAGE_FLAG_PING)
//   uint8_t payload[size]  (not present if NET_MESSAGE_FLAG_PING)
//
// Reliable messages are split into one or more fragments. Every fragment gets the next ID from a per-connection counter and the receiver
// delivers fragments strictly in ID order, so a message is just a run of fragments ending with one that doesn't have MORE_FRAGMENTS set.
//
// A packet that contains at least one message asks the receiver to acknowledge it. Packets containing no messages are only
// acknowledgements themselves and never trigger a reply, otherwise two idle hosts would acknowledge each other forever.
static constexpr uint8_t NET_MESSAGE_FLAG_RELIABLE = 1 << 0;
static constexpr uint8_t NET_MESSAGE_FLAG_MORE_FRAGMENTS = 1 << 1; // only valid with RELIABLE
static constexpr uint8_t NET_MESSAGE_FLAG_PING = 1 << 2;           // an empty message that only exists to request an acknowledgement
static constexpr uint8_t NET_MESSAGE_FLAGS_ALL = NET_MESSAGE_FLAG_RELIABLE | NET_MESSAGE_FLAG_MORE_FRAGMENTS | NET_MESSAGE_FLAG_PING;

static constexpr auto ACK_DELAY_UNIT = std::chrono::microseconds(100);

NetConnection::NetConnection(NetClock::time_point now) : m_last_send_time(now), m_last_receive_time(now), m_rate_time(now)
{
    m_stats.rto_ms = static_cast<float>(m_rto_calc.getRTOMilliseconds());
}

bool NetConnection::queueMessage(NetDelivery delivery, std::span<const uint8_t> message)
{
    if (delivery == NetDelivery::UNRELIABLE) {
        if (message.size() > MAX_UNRELIABLE_SIZE) {
            GC_ERROR_ONCE("Unreliable net message of {} bytes is too large to send (max {}). Send it reliably instead.", message.size(), MAX_UNRELIABLE_SIZE);
            ++m_stats.unreliable_dropped;
            return true;
        }
        if (m_unreliable_queue.size() >= MAX_QUEUED_UNRELIABLE) {
            m_unreliable_queue.pop_front();
            ++m_stats.unreliable_dropped;
        }
        m_unreliable_queue.emplace_back(message.begin(), message.end());
        return true;
    }

    if (message.size() > NET_MAX_MESSAGE_SIZE) {
        GC_ERROR("Reliable net message of {} bytes is too large to send (max {})", message.size(), NET_MAX_MESSAGE_SIZE);
        return true;
    }

    const size_t num_fragments = std::max<size_t>(1, (message.size() + MAX_FRAGMENT_SIZE - 1) / MAX_FRAGMENT_SIZE);
    if (m_send_fragments.size() + num_fragments > MAX_QUEUED_FRAGMENTS) {
        return false;
    }

    for (size_t i = 0; i < num_fragments; ++i) {
        const size_t offset = i * MAX_FRAGMENT_SIZE;
        const size_t size = std::min(MAX_FRAGMENT_SIZE, message.size() - offset);
        SendFragment& fragment = m_send_fragments.emplace_back();
        fragment.data.assign(message.begin() + offset, message.begin() + offset + size);
        fragment.more_fragments = (i + 1 < num_fragments);
    }
    ++m_stats.reliable_sent;
    return true;
}

bool NetConnection::writePacket(ByteWriter& writer, NetClock::time_point now)
{
    GC_ASSERT(writer.remaining() >= NetPacketData::getSerialisedSize() + MAX_PACKET_PAYLOAD);

    const bool keepalive_due = (now - m_last_send_time >= KEEPALIVE_INTERVAL);
    const bool ack_due = m_ack_pending && (m_ack_immediate || m_ack_pending_count >= ACK_MAX_PENDING || now - m_ack_pending_since >= ACK_MAX_DELAY);
    detectLosses(now);

    bool has_due_fragment = false;
    size_t fragment_allowance = getSendAllowance(now, has_due_fragment);
    const bool data_due = !m_unreliable_queue.empty() || (has_due_fragment && fragment_allowance > 0);
    if (!keepalive_due && !ack_due && !data_due) {
        return false;
    }

    NetPacketData header{};
    header.seq_num = m_next_seq_num;
    header.ack_num = m_ack_num;
    for (size_t i = 0; i < header.ack_bits.size(); ++i) {
        header.ack_bits.set(i, m_received_bits.test(i));
    }
    if (m_stats.packets_received > 0) {
        const auto delay = (now - m_ack_num_receive_time) / ACK_DELAY_UNIT;
        header.ack_delay = static_cast<uint16_t>(std::clamp<int64_t>(delay, 0, UINT16_MAX));
    }
    header.serialise(writer);

    SentPacket& sent_packet = m_sent_packets[header.seq_num % m_sent_packets.size()];
    sent_packet.send_time = now;
    sent_packet.fragment_ids.clear();
    sent_packet.seq_num = header.seq_num;
    sent_packet.valid = true;
    sent_packet.acked = false;

    size_t budget = MAX_PACKET_PAYLOAD;
    bool has_message = false;

    // Reliable fragments go first. Fragments that don't fit are skipped as a later (smaller) one might.
    const size_t window = std::min(m_send_fragments.size(), WINDOW_SIZE);
    for (size_t i = 0; i < window && budget > RELIABLE_HEADER_SIZE && fragment_allowance > 0; ++i) {
        SendFragment& fragment = m_send_fragments[i];
        if (!isFragmentDue(fragment, now) || RELIABLE_HEADER_SIZE + fragment.data.size() > budget) {
            continue;
        }
        if (fragment.send_count > 0 && !fragment.lost) {
            onCongestion(now); // the retransmission timeout expired
        }
        // The last fragment is allowed to go over, otherwise a nearly used up window would only let small fragments through
        fragment_allowance -= std::min(fragment_allowance, RELIABLE_HEADER_SIZE + fragment.data.size());
        const uint16_t fragment_id = static_cast<uint16_t>(m_send_base + i);
        writer.writeU8(fragment.more_fragments ? static_cast<uint8_t>(NET_MESSAGE_FLAG_RELIABLE | NET_MESSAGE_FLAG_MORE_FRAGMENTS) : NET_MESSAGE_FLAG_RELIABLE);
        writer.writeU16(fragment_id);
        writer.writeU16(static_cast<uint16_t>(fragment.data.size()));
        writer.writeBytes(fragment.data);
        budget -= RELIABLE_HEADER_SIZE + fragment.data.size();

        if (fragment.send_count > 0) {
            ++m_stats.reliable_resent;
        }
        if (fragment.send_count < UINT8_MAX) {
            ++fragment.send_count;
        }
        fragment.last_send_time = now;
        fragment.last_packet_seq = header.seq_num;
        fragment.lost = false;
        sent_packet.fragment_ids.push_back(fragment_id);
        has_message = true;
    }

    while (!m_unreliable_queue.empty()) {
        const std::vector<uint8_t>& message = m_unreliable_queue.front();
        if (UNRELIABLE_HEADER_SIZE + message.size() > budget) {
            break; // goes in the next packet
        }
        writer.writeU8(0);
        writer.writeU16(static_cast<uint16_t>(message.size()));
        writer.writeBytes(message);
        budget -= UNRELIABLE_HEADER_SIZE + message.size();
        m_unreliable_queue.pop_front();
        ++m_stats.unreliable_sent;
        has_message = true;
    }

    if (!has_message && keepalive_due) {
        // Nothing has been sent for a while. Ask for an acknowledgement so that both hosts know the other is still there.
        writer.writeU8(NET_MESSAGE_FLAG_PING);
    }

    sent_packet.has_message = has_message;

    ++m_next_seq_num;
    m_last_send_time = now;
    m_ack_pending = false;
    m_ack_immediate = false;
    m_ack_pending_count = 0;
    ++m_stats.packets_sent;
    m_stats.bytes_sent += writer.pos();
    return true;
}

void NetConnection::receivePacket(ByteReader& reader, NetClock::time_point now, std::vector<std::vector<uint8_t>>& out_messages)
{
    const size_t packet_size = reader.pos() + reader.remaining();

    const auto header = tryDeserialise<NetPacketData>(reader);
    if (!header) {
        ++m_stats.packets_malformed;
        return;
    }

    // Anything with a valid session token shows that the remote host is still there
    m_last_receive_time = now;
    ++m_stats.packets_received;
    m_stats.bytes_received += packet_size;

    const int16_t diff = seq_diff(header->seq_num, m_ack_num);
    if (diff > 0) {
        // new sequence number. slide window forward.
        m_received_bits <<= diff;
        m_received_bits.set(0, true);
        m_ack_num = header->seq_num;
        m_ack_num_receive_time = now;
    }
    else if (diff > -static_cast<int16_t>(m_received_bits.size())) {
        if (m_received_bits.test(-diff)) {
            ++m_stats.packets_duplicate;
            return;
        }
        // previously missing sequence number, acknowledge it.
        m_received_bits.set(-diff, true);
    }
    else {
        // Too old to tell if it's a duplicate. Any reliable fragments it contains will be sent again.
        ++m_stats.packets_duplicate;
        return;
    }

    processAcks(*header, now);

    bool has_message = false;
    bool has_reliable = false;
    while (reader.remaining() > 0) {
        const uint8_t flags = reader.readU8();
        if ((flags & static_cast<uint8_t>(~NET_MESSAGE_FLAGS_ALL)) != 0) {
            ++m_stats.packets_malformed;
            break;
        }
        has_message = true;
        if (flags & NET_MESSAGE_FLAG_PING) {
            continue;
        }
        const bool reliable = (flags & NET_MESSAGE_FLAG_RELIABLE) != 0;
        if (reader.remaining() < (reliable ? 2 * sizeof(uint16_t) : sizeof(uint16_t))) {
            ++m_stats.packets_malformed;
            break;
        }
        const uint16_t fragment_id = reliable ? reader.readU16() : uint16_t{0};
        const uint16_t size = reader.readU16();
        if (reader.remaining() < size) {
            ++m_stats.packets_malformed;
            break;
        }
        if (reliable) {
            has_reliable = true;
            receiveFragment(reader, flags, fragment_id, size);
        }
        else {
            std::vector<uint8_t>& message = out_messages.emplace_back(size);
            reader.readBytes(message);
            ++m_stats.unreliable_received;
        }
    }

    if (has_reliable) {
        deliverFragments(out_messages);
    }

    if (has_message) {
        if (!m_ack_pending) {
            m_ack_pending = true;
            m_ack_pending_since = now;
        }
        ++m_ack_pending_count;
        // The remote host can't send beyond its window until reliable fragments are acknowledged, so don't hold those back
        m_ack_immediate |= has_reliable;
    }
}

bool NetConnection::hasTimedOut(NetClock::time_point now) const { return now - m_last_receive_time > TIMEOUT; }

void NetConnection::updateStats(NetClock::time_point now)
{
    m_stats.idle_ms = std::chrono::duration<float, std::milli>(now - m_last_receive_time).count();
    m_stats.congestion_window = m_congestion_window;
    m_stats.reliable_queue = static_cast<uint32_t>(m_send_fragments.size());
    m_stats.unreliable_queue = static_cast<uint32_t>(m_unreliable_queue.size());

    if (const auto elapsed = now - m_rate_time; elapsed >= RATE_INTERVAL) {
        const float seconds = std::chrono::duration<float>(elapsed).count();
        m_stats.send_rate = static_cast<float>(m_stats.bytes_sent - m_rate_bytes_sent) / seconds;
        m_stats.receive_rate = static_cast<float>(m_stats.bytes_received - m_rate_bytes_received) / seconds;
        m_rate_time = now;
        m_rate_bytes_sent = m_stats.bytes_sent;
        m_rate_bytes_received = m_stats.bytes_received;
    }
}

const NetConnectionStats& NetConnection::getStats() const { return m_stats; }

bool NetConnection::isFragmentDue(const SendFragment& fragment, NetClock::time_point now) const
{
    if (fragment.acked) {
        return false;
    }
    if (fragment.send_count == 0 || fragment.lost) {
        return true;
    }
    return now - fragment.last_send_time >= m_rto_calc.getRTOWithBackoff(fragment.send_count);
}

size_t NetConnection::getSendAllowance(NetClock::time_point now, bool& has_due) const
{
    has_due = false;
    size_t in_flight = 0; // bytes
    const size_t window = std::min(m_send_fragments.size(), WINDOW_SIZE);
    for (size_t i = 0; i < window; ++i) {
        const SendFragment& fragment = m_send_fragments[i];
        if (isFragmentDue(fragment, now)) {
            has_due = true; // either never sent, or given up on
        }
        else if (!fragment.acked) {
            in_flight += RELIABLE_HEADER_SIZE + fragment.data.size();
        }
    }
    const size_t congestion_window = static_cast<size_t>(m_congestion_window * static_cast<float>(MAX_PACKET_PAYLOAD));
    return (in_flight < congestion_window) ? (congestion_window - in_flight) : 0;
}

void NetConnection::processAcks(const NetPacketData& header, NetClock::time_point now)
{
    // uint16_t subtraction underflow is well defined.
    for (uint16_t i = 0; i < static_cast<uint16_t>(header.ack_bits.size()); ++i) {
        if (!header.ack_bits.test(i)) {
            continue;
        }
        const uint16_t seq_num = header.ack_num - i;
        SentPacket& sent_packet = m_sent_packets[seq_num % m_sent_packets.size()];
        if (!sent_packet.valid || sent_packet.seq_num != seq_num || sent_packet.acked) {
            continue;
        }
        sent_packet.acked = true;
        if (sent_packet.has_message) {
            ++m_stats.packets_acked;
            recordPacketOutcome(false);
        }

        if (i == 0) {
            // ack_delay only describes ack_num, so that's the only packet an accurate RTT can be taken from.
            // Packets are never retransmitted with the same sequence number so there is no ambiguity about which send was acknowledged.
            auto rtt = now - sent_packet.send_time;
            const auto ack_delay = std::chrono::duration_cast<NetClock::duration>(ACK_DELAY_UNIT * header.ack_delay);
            if (rtt > ack_delay) {
                rtt -= ack_delay;
            }
            m_rto_calc.recordRTT(rtt);
            m_latest_rtt = rtt;
            m_stats.rtt_ms = std::chrono::duration<float, std::milli>(rtt).count();
            m_stats.smoothed_rtt_ms = static_cast<float>(m_rto_calc.getSmoothedRTTMilliseconds());
            m_stats.rto_ms = static_cast<float>(m_rto_calc.getRTOMilliseconds());
        }

        for (const uint16_t fragment_id : sent_packet.fragment_ids) {
            // IDs older than m_send_base wrap around to a large index
            const size_t index = static_cast<uint16_t>(fragment_id - m_send_base);
            if (index < m_send_fragments.size() && !m_send_fragments[index].acked) {
                m_send_fragments[index].acked = true;
                // slow start (doubles every round trip) until the threshold, then one more packet every round trip
                const float acked_packets = static_cast<float>(RELIABLE_HEADER_SIZE + m_send_fragments[index].data.size()) / static_cast<float>(MAX_PACKET_PAYLOAD);
                m_congestion_window += (m_congestion_window < m_slow_start_threshold) ? acked_packets : (acked_packets / m_congestion_window);
                m_congestion_window = std::min(m_congestion_window, static_cast<float>(WINDOW_SIZE));
            }
        }

        if (!m_any_acked || seq_diff(seq_num, m_highest_acked) > 0) {
            m_highest_acked = seq_num;
            m_any_acked = true;
        }
    }

    while (!m_send_fragments.empty() && m_send_fragments.front().acked) {
        m_send_fragments.pop_front();
        ++m_send_base;
    }

    detectLosses(now);
}

// A packet is considered lost if newer packets have been acknowledged and it has been unacknowledged for longer than a round
// trip should take. Waiting that long (instead of only counting newer acknowledgements) avoids mistaking reordering for loss.
// Fragments in lost packets are resent straight away instead of waiting for their retransmission timeout.
void NetConnection::detectLosses(NetClock::time_point now)
{
    if (!m_any_acked) {
        return;
    }

    // includes the time the remote host is allowed to hold back an acknowledgement for
    const auto loss_delay = std::max(m_rto_calc.getSmoothedRTT(), m_latest_rtt) * 5 / 4 + ACK_MAX_DELAY * 2;

    if (seq_diff(m_highest_acked, m_loss_scan_seq_num) > static_cast<int16_t>(m_sent_packets.size())) {
        m_loss_scan_seq_num = m_highest_acked - static_cast<uint16_t>(m_sent_packets.size()); // older packets are no longer in the history
    }
    while (seq_diff(m_highest_acked, m_loss_scan_seq_num) >= LOSS_REORDER_THRESHOLD) {
        const SentPacket& sent_packet = m_sent_packets[m_loss_scan_seq_num % m_sent_packets.size()];
        if (sent_packet.valid && sent_packet.seq_num == m_loss_scan_seq_num && !sent_packet.acked) {
            if (now - sent_packet.send_time < loss_delay) {
                break; // Might just be late. Every packet after this one was sent even more recently, so check again later.
            }
            if (sent_packet.has_message) {
                ++m_stats.packets_lost;
                recordPacketOutcome(true);
            }
            for (const uint16_t fragment_id : sent_packet.fragment_ids) {
                const size_t index = static_cast<uint16_t>(fragment_id - m_send_base);
                // only if the fragment hasn't already been sent again in a newer packet
                if (index < m_send_fragments.size() && !m_send_fragments[index].acked && m_send_fragments[index].last_packet_seq == sent_packet.seq_num) {
                    m_send_fragments[index].lost = true;
                    onCongestion(now);
                }
            }
        }
        ++m_loss_scan_seq_num;
    }
}

void NetConnection::onCongestion(NetClock::time_point now)
{
    // Everything lost within one round trip is the same congestion event, so only react to it once
    const auto min_interval = std::max<NetClock::duration>(m_rto_calc.getSmoothedRTT(), std::chrono::milliseconds(50));
    if (now - m_last_congestion_time < min_interval) {
        return;
    }
    m_last_congestion_time = now;
    m_slow_start_threshold = std::max(m_congestion_window * 0.5f, CONGESTION_WINDOW_MIN);
    m_congestion_window = m_slow_start_threshold;
}

void NetConnection::receiveFragment(ByteReader& reader, uint8_t flags, uint16_t fragment_id, uint16_t size)
{
    const int16_t diff = seq_diff(fragment_id, m_receive_base);
    if (diff < 0 || diff >= static_cast<int16_t>(m_receive_fragments.size())) {
        // Already delivered, or beyond the window (which a well behaved sender never does)
        reader.skip(size);
        return;
    }
    ReceiveFragment& fragment = m_receive_fragments[fragment_id % m_receive_fragments.size()];
    if (fragment.present) {
        reader.skip(size);
        return;
    }
    fragment.data.resize(size);
    reader.readBytes(fragment.data);
    fragment.present = true;
    fragment.more_fragments = (flags & NET_MESSAGE_FLAG_MORE_FRAGMENTS) != 0;
}

void NetConnection::deliverFragments(std::vector<std::vector<uint8_t>>& out_messages)
{
    for (;;) {
        ReceiveFragment& fragment = m_receive_fragments[m_receive_base % m_receive_fragments.size()];
        if (!fragment.present) {
            break;
        }

        if (!m_reassembling && !fragment.more_fragments) {
            // the whole message is in this fragment
            out_messages.push_back(std::move(fragment.data));
            ++m_stats.reliable_received;
        }
        else {
            m_reassembling = true;
            if (m_reassembly.size() + fragment.data.size() > NET_MAX_MESSAGE_SIZE) {
                // Never sent by this implementation. Discard the rest of the message instead of buffering without limit.
                m_reassembly_overflow = true;
                m_reassembly.clear();
            }
            if (!m_reassembly_overflow) {
                m_reassembly.insert(m_reassembly.end(), fragment.data.begin(), fragment.data.end());
            }
            if (!fragment.more_fragments) {
                if (m_reassembly_overflow) {
                    ++m_stats.packets_malformed;
                }
                else {
                    out_messages.push_back(std::move(m_reassembly));
                    ++m_stats.reliable_received;
                }
                m_reassembly.clear();
                m_reassembling = false;
                m_reassembly_overflow = false;
            }
        }

        fragment.data.clear();
        fragment.present = false;
        ++m_receive_base;
    }
}

void NetConnection::recordPacketOutcome(bool lost)
{
    constexpr float SMOOTHING = 1.0f / 32.0f;
    m_stats.packet_loss += ((lost ? 1.0f : 0.0f) - m_stats.packet_loss) * SMOOTHING;
}

} // namespace gc
