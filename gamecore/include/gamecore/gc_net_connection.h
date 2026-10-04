#pragma once

// The state of a connection with one remote host. Used by both NetClient and NetServer.
// Implements everything that happens inside DATA packets:
//  - packet sequence numbers and acknowledgements
//  - round trip time measurement, loss detection and congestion control
//  - unreliable messages
//  - reliable ordered messages, with retransmission and fragmentation of large messages
//  - batching of multiple messages into a single packet
//  - keepalives and idle timeout
//
// This class does no I/O itself. The owner passes received packets to receivePacket() and sends whatever writePacket() produces.
// This class is not thread safe

#include <cstddef>
#include <cstdint>

#include <array>
#include <bitset>
#include <deque>
#include <span>
#include <vector>

#include "gamecore/gc_net_common.h"
#include "gamecore/gc_net_rto.h"

namespace gc {

class NetConnection {
public:
    // sizes of the headers that precede a message's payload inside a DATA packet
    static constexpr size_t UNRELIABLE_HEADER_SIZE = 1 + 2;   // flags, size
    static constexpr size_t RELIABLE_HEADER_SIZE = 1 + 2 + 2; // flags, fragment id, size

    static constexpr size_t MAX_PACKET_PAYLOAD = NET_MAX_PACKET_SIZE - NetPacketHeader::getSerialisedSize() - NetPacketData::getSerialisedSize();
    static constexpr size_t MAX_UNRELIABLE_SIZE = MAX_PACKET_PAYLOAD - UNRELIABLE_HEADER_SIZE;
    static constexpr size_t MAX_FRAGMENT_SIZE = MAX_PACKET_PAYLOAD - RELIABLE_HEADER_SIZE;

private:
    struct SendFragment {
        std::vector<uint8_t> data{};
        NetClock::time_point last_send_time{};
        uint16_t last_packet_seq{}; // the most recent packet this fragment was sent in
        uint8_t send_count{};       // 0 if it has never been sent
        bool more_fragments{};      // the message continues in the next fragment
        bool acked{};
        bool lost{}; // the most recent packet this fragment was sent in was lost, so resend it without waiting for the RTO
    };

    struct ReceiveFragment {
        std::vector<uint8_t> data{};
        bool present{};
        bool more_fragments{};
    };

    struct SentPacket {
        NetClock::time_point send_time{};
        std::vector<uint16_t> fragment_ids{}; // the reliable fragments this packet carried
        uint16_t seq_num{};
        bool valid{};
        bool acked{};
        bool has_message{}; // packets without messages are only acknowledged by chance, so they say nothing about packet loss
    };

    static constexpr size_t WINDOW_SIZE = 256;            // how far ahead of the oldest unacknowledged fragment can be sent
    static constexpr size_t MAX_QUEUED_FRAGMENTS = 8192;  // must be less than 32768
    static constexpr size_t MAX_QUEUED_UNRELIABLE = 1024; // older messages are dropped first
    static constexpr size_t SENT_PACKET_HISTORY = 256;
    static constexpr size_t RECEIVED_PACKET_HISTORY = 256; // packets that arrive later than this many sequence numbers are discarded
    static constexpr int16_t LOSS_REORDER_THRESHOLD = 3;   // a packet can only be lost once this many newer packets have been acknowledged
    static constexpr float CONGESTION_WINDOW_INITIAL = 32.0f;
    static constexpr float CONGESTION_WINDOW_MIN = 16.0f;
    static constexpr uint32_t ACK_MAX_PENDING = 8;       // send an acknowledgement once this many packets are waiting for one
    static constexpr auto ACK_MAX_DELAY = std::chrono::milliseconds(25);
    static constexpr auto KEEPALIVE_INTERVAL = std::chrono::milliseconds(500);
    static constexpr auto TIMEOUT = std::chrono::seconds(5);
    static constexpr auto RATE_INTERVAL = std::chrono::milliseconds(500);

    // sending packets
    std::array<SentPacket, SENT_PACKET_HISTORY> m_sent_packets{}; // indexed by sequence number
    uint16_t m_next_seq_num{0};                                   // post-incremented when sending
    uint16_t m_highest_acked{0};
    uint16_t m_loss_scan_seq_num{0}; // the oldest sent packet that is yet to be checked for loss
    bool m_any_acked{false};
    NetClock::time_point m_last_send_time{};

    // receiving packets
    uint16_t m_ack_num{UINT16_MAX}; // the highest received sequence number. init to 65535
    // Which of the most recent remote sequence numbers have been received. Bit N is (m_ack_num - N). init to all 1s
    // Only the lowest bits fit in a packet's ack_bits. The rest are for recognising duplicates of packets that arrive very late.
    std::bitset<RECEIVED_PACKET_HISTORY> m_received_bits{std::bitset<RECEIVED_PACKET_HISTORY>{}.set()};
    NetClock::time_point m_ack_num_receive_time{};
    NetClock::time_point m_last_receive_time{};
    bool m_ack_pending{false};   // packets have been received that the remote host wants an acknowledgement for
    bool m_ack_immediate{false}; // the acknowledgement shouldn't be delayed
    uint32_t m_ack_pending_count{0};
    NetClock::time_point m_ack_pending_since{};

    // sending messages
    std::deque<SendFragment> m_send_fragments{}; // the front has ID m_send_base
    uint16_t m_send_base{0};
    std::deque<std::vector<uint8_t>> m_unreliable_queue{};

    // Congestion control. Limits how much reliable data can be waiting for an acknowledgement at once.
    // Grows as fragments are acknowledged and halves when they are lost (the same idea as TCP's AIMD).
    // Measured in full packets, so lots of small messages cost no more than the packets they share.
    float m_congestion_window{CONGESTION_WINDOW_INITIAL};
    float m_slow_start_threshold{static_cast<float>(WINDOW_SIZE)};
    NetClock::time_point m_last_congestion_time{};
    NetClock::duration m_latest_rtt{};

    // receiving messages
    std::array<ReceiveFragment, WINDOW_SIZE> m_receive_fragments{}; // indexed by fragment ID
    uint16_t m_receive_base{0};                                     // the next fragment ID to deliver
    std::vector<uint8_t> m_reassembly{};
    bool m_reassembling{false};
    bool m_reassembly_overflow{false};

    RetransmitTimeoutCalculator m_rto_calc{};

    NetConnectionStats m_stats{};
    NetClock::time_point m_rate_time{};
    uint64_t m_rate_bytes_sent{};
    uint64_t m_rate_bytes_received{};

public:
    explicit NetConnection(NetClock::time_point now);

    // Queues a message to be sent by later calls to writePacket().
    // Returns false if the reliable send queue is full, in which case the connection can no longer guarantee delivery and should be closed.
    [[nodiscard]] bool queueMessage(NetDelivery delivery, std::span<const uint8_t> message);

    // Writes the contents of a DATA packet (not including NetPacketHeader) if there is something that needs to be sent right now.
    // The writer must have MAX_PACKET_PAYLOAD + NetPacketData::getSerialisedSize() bytes remaining.
    // Returns false if nothing was written. Call repeatedly until it returns false.
    [[nodiscard]] bool writePacket(ByteWriter& writer, NetClock::time_point now);

    // Reads the contents of a DATA packet (the reader should be positioned after NetPacketHeader).
    // Any messages that are now ready for delivery are appended to out_messages.
    void receivePacket(ByteReader& reader, NetClock::time_point now, std::vector<std::vector<uint8_t>>& out_messages);

    bool hasTimedOut(NetClock::time_point now) const;

    // Call periodically to keep rates and timers in the stats up to date
    void updateStats(NetClock::time_point now);
    const NetConnectionStats& getStats() const;

private:
    bool isFragmentDue(const SendFragment& fragment, NetClock::time_point now) const;

    // Returns how many more bytes of fragments the congestion window allows to be sent.
    // has_due is set if any fragment is waiting to be sent.
    size_t getSendAllowance(NetClock::time_point now, bool& has_due) const;
    void processAcks(const NetPacketData& header, NetClock::time_point now);
    void detectLosses(NetClock::time_point now);
    void onCongestion(NetClock::time_point now);
    void receiveFragment(ByteReader& reader, uint8_t flags, uint16_t fragment_id, uint16_t size);
    void deliverFragments(std::vector<std::vector<uint8_t>>& out_messages);
    void recordPacketOutcome(bool lost);
};

} // namespace gc
