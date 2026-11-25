#pragma once

#include "core/app_clock.hpp"
#include "network/packet.hpp"

#include <chrono>
#include <cstdint>

namespace wowee::game {

/// How long a packet has waited since the socket read it, in milliseconds.
///
/// The client has no world to load when the realm's packets come: it loads the
/// character's before it sends CMSG_PLAYER_LOGIN (0x004dab40, state 10:
/// 0x00405540, and 0x006b1790 the next time round) and a new one before
/// MSG_MOVE_WORLDPORT_ACK (0x00403b70). Here the world is loaded once the realm
/// has the character in it, and what comes meanwhile waits: a flight began as
/// it was handled, six seconds behind the server's. 0 for a packet made here,
/// and past any load.
[[nodiscard]] inline uint32_t packetWaitedMs(const network::Packet& packet) {
    if (packet.receivedMs() <= 0) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        core::steadyNow().time_since_epoch()).count();
    const int64_t waited = static_cast<int64_t>(now) - packet.receivedMs();
    return waited > 0 && waited <= 600000 ? static_cast<uint32_t>(waited) : 0u;
}

}  // namespace wowee::game
