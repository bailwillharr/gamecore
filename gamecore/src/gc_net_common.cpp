#include "gamecore/gc_net_common.h"

#include <cstring>
#include <cstdint>

#include "gamecore/gc_assert.h"

namespace gc {

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
    case NetPacketType::MESSAGE:
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

} // namespace gc
