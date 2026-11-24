#pragma once

// What the movement state is told of the followed character after a step.
//
// The client keeps one state: FALLING (+0x44 0x1000) and SWIMMING (0x200000)
// are what its step goes by, set as a fall or a swim starts (0x00988370,
// 0x00989660) and cleared as it ends (0x00988490, 0x0098bff0), whether or not
// a packet goes out for it. Here the step and the flags are two things, and
// told apart by an edge of the step's - the frame it landed in, the frame it
// first swam in - one missed is missed for good: FALLING stayed set on a
// character standing on the ground, which casts nothing then, and a swimmer
// logged in afloat never had SWIMMING. So what the flags say is asked each
// time, and made to say what the step did.

#include <cstdint>

namespace wowee {
namespace rendering {

/// Where a step left the mover, and what its movement flags say.
struct MoverStep {
    bool onGround = false;
    bool swimming = false;
    bool flying = false;
    /// Gravity is off (0x400): it neither falls nor lands.
    bool floating = false;
    /// Carried by something else - a flight path, a server's spline - and not
    /// stepped at all.
    bool carried = false;
    bool saidFalling = false;
    bool saidSwimming = false;
};

enum class FallChange : uint8_t {
    None,
    Began,   ///< The ground gone from under it without a jump: FALLING on.
    Landed,  ///< On ground with FALLING on, however it came there.
    Over,    ///< FALLING on where nothing falls: over, with no landing said.
};

[[nodiscard]] constexpr FallChange fallChange(const MoverStep& s) noexcept {
    if (s.carried || s.floating || s.swimming || s.flying) {
        return s.saidFalling ? FallChange::Over : FallChange::None;
    }
    if (s.onGround) return s.saidFalling ? FallChange::Landed : FallChange::None;
    return s.saidFalling ? FallChange::None : FallChange::Began;
}

enum class SwimChange : uint8_t { None, Starts, Stops };

[[nodiscard]] constexpr SwimChange swimChange(const MoverStep& s) noexcept {
    if (s.carried || s.swimming == s.saidSwimming) return SwimChange::None;
    return s.swimming ? SwimChange::Starts : SwimChange::Stops;
}

}  // namespace rendering
}  // namespace wowee
