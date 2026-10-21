#pragma once

// What a movement event of the active player's own does to its animation.
//
// The client handles each event of the unit it moves (0x00741xxx: the keys,
// a jump, the acks of what the server forces on it) as it does each one of
// another unit's packets, and ends it in the same switch (0x0073ed10): a
// start, a stop, a turn, a dive, a climb, a root or a teleport asks for the
// unit's base animation again (0x0073ac30); a change of walk or run mode or
// of a speed asks it only while the unit moves; a jump plays JumpStart, a
// knockback Fall; taking off into flight plays JumpStart and landing from it
// JumpLandRun. A start - forward, back, a strafe, a swim, a climb or a dive -
// and a jump first let go of a bow pulled (0x0072afe0).

#include "game/opcodes.hpp"

#include <cstdint>

namespace wowee {
namespace rendering {
namespace mover_event {

enum class Play : uint8_t {
    Nothing,
    Base,         // 0x0073ac30
    JumpStart,    // 0x25
    Fall,         // 0x28
    JumpLandRun,  // 0xbb, down from flight
};

struct Reaction {
    Play play = Play::Nothing;
    /// 0x0072afe0: the bow pulled (+0xa38 0x4000) let go.
    bool releasesBow = false;
};

/// 0x0073ed10 for an event of the active player's. `flags` are its movement
/// flags after the event; `fallPlaying` whether Fall is what its body or
/// upper body plays (0x007172e0, 0x00717260); `seatHolds` whether the seat
/// it rides in keeps it from a jump's animations (0x004f6210 ->
/// 0x007571c0).
[[nodiscard]] Reaction react(game::Opcode op, uint32_t flags, bool fallPlaying, bool seatHolds);

}  // namespace mover_event
}  // namespace rendering
}  // namespace wowee
