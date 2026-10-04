#include "gamecore/gc_net_common.h"

#include <cstdint>
#include <cstring>

#include <random>

#include "gamecore/gc_assert.h"

namespace gc {

void NetEventQueue::push(NetEvent event)
{
    std::scoped_lock lock(m_mutex);
    m_queue.push(std::move(event));
}

bool NetEventQueue::pop(NetEvent& ev)
{
    std::scoped_lock lock(m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    else {
        ev = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }
}

bool verifyPacketHeader(const NetPacketHeader& header)
{
    if (header.magic != NET_PACKET_MAGIC) {
        return false;
    }
    if (header.version != NET_PACKET_VERSION) {
        return false;
    }
    switch (header.type) {
    case NetPacketType::CONNECT_REQUEST:
        if (header.token != 0) {
            return false;
        }
        break;
    case NetPacketType::CONNECT_CHALLENGE:
    case NetPacketType::CONNECT_CHALLENGE_RESPONSE:
    case NetPacketType::CONNECT_ACCEPT:
    case NetPacketType::DATA:
    case NetPacketType::DISCONNECT:
        if (header.token == 0) {
            return false;
        }
        break;
    default:
        return false;
    }
    return true;
}

// returns positive: a is newer than b
// returns negative: a is older than b
int16_t seq_diff(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b); }

const char* netDisconnectReasonString(NetDisconnectReason reason)
{
    switch (reason) {
    case NetDisconnectReason::NONE:
        return "none";
    case NetDisconnectReason::LOCAL_REQUEST:
        return "closed locally";
    case NetDisconnectReason::REMOTE_CLOSED:
        return "closed by remote host";
    case NetDisconnectReason::TIMED_OUT:
        return "timed out";
    case NetDisconnectReason::KICKED:
        return "kicked";
    case NetDisconnectReason::SERVER_FULL:
        return "server full";
    case NetDisconnectReason::SERVER_SHUTDOWN:
        return "server shut down";
    case NetDisconnectReason::CONNECT_FAILED:
        return "no response from server";
    case NetDisconnectReason::SEND_QUEUE_OVERFLOW:
        return "send queue overflow";
    case NetDisconnectReason::SOCKET_FAILED:
        return "socket error";
    }
    return "unknown";
}

std::vector<uint8_t> encodeNetEvent(const NetEvent& ev)
{
    std::vector<uint8_t> message(sizeof(uint32_t) + ev.data.size());
    ByteWriter writer(message);
    writer.writeU32(ev.type.getHash());
    writer.writeBytes(ev.data);
    return message;
}

std::optional<NetEvent> decodeNetEvent(std::vector<uint8_t>&& message, NetPeerId peer)
{
    if (message.size() < sizeof(uint32_t)) {
        return std::nullopt;
    }
    ByteReader reader(message);
    NetEvent ev{};
    ev.type = Name(reader.readU32());
    ev.kind = NetEventKind::MESSAGE;
    ev.peer = peer;
    // reuse the message's allocation for the event data
    message.erase(message.begin(), message.begin() + sizeof(uint32_t));
    ev.data = std::move(message);
    return ev;
}

uint64_t generateNetRandom64()
{
    static_assert(std::random_device::max() == UINT32_MAX && std::random_device::min() == 0);
    std::random_device rd{};
    const uint64_t high = rd();
    const uint64_t low = rd();
    return (high << 32) | low;
}

} // namespace gc
