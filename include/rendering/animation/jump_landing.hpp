#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

/// How a unit touches down from a jump or a long fall.
enum class JumpLanding : uint8_t {
    None,       // Straight back to its locomotion
    End,        // JumpEnd (39), then its rest animation
    LandRun,    // JumpLandRun (187), then Run
};

/// The client's landing (FUN_0073d2b0), for the local player and for every
/// other unit alike: with no direction held it plays JumpEnd; running on -
/// forward or strafing, not backing up, and faster than twice walk speed
/// (FUN_00716fa0 answers no landing at or under that) - JumpLandRun; walking
/// or backing up goes straight back to moving.
[[nodiscard]] constexpr JumpLanding chooseJumpLanding(bool moving, bool backward,
                                                      bool running) noexcept {
    if (!moving) return JumpLanding::End;
    return !backward && running ? JumpLanding::LandRun : JumpLanding::None;
}

/// The same landing from the movement flags the unit lands with: the
/// direction bits (& 0xF), backward (0x2) and walking (0x100) as
/// FUN_0073d2b0 tests them, and the speed those flags move it at
/// (CMovement::GetCurrentSpeed, FUN_00987570) against twice its walk speed
/// (FUN_00716fa0).
[[nodiscard]] constexpr JumpLanding jumpLandingForFlags(uint32_t moveFlags, float currentSpeed,
                                                        float walkSpeed) noexcept {
    return chooseJumpLanding((moveFlags & 0xFu) != 0, (moveFlags & 0x2u) != 0,
                             (moveFlags & 0x100u) == 0 && currentSpeed > walkSpeed + walkSpeed);
}

/// What the per-frame sync of another unit's animation does about its jump.
enum class AirborneAnimSync : uint8_t {
    None,       // Nothing to do with a jump: pick locomotion as usual
    Hold,       // Leave the jump animation playing
    Fall,       // Airborne without a jump animation: play Fall (40)
    Refresh,    // Down again but still in an airborne loop: pick locomotion now
};

/// While a unit is airborne - falling (0x1000) after a jump or knockback, or
/// falling far (0x2000), as FUN_00723350 has it - the client's locomotion
/// choice (FUN_00724200) keeps whichever of JumpStart, Jump, JumpEnd and Fall
/// (37-40) is playing (FUN_0071dc20) and otherwise plays Fall; a JumpStart
/// gives way to the Jump loop when it ends (FUN_0073b510). Once down, the
/// landing one-shot (JumpEnd, JumpLandRun) plays out; a unit still in an
/// airborne loop (a landing that plays nothing) goes back to its locomotion.
[[nodiscard]] constexpr AirborneAnimSync airborneAnimSync(bool airborne,
                                                          uint32_t currentAnim) noexcept {
    const bool inAirLoop = currentAnim == anim::JUMP_START || currentAnim == anim::JUMP ||
                           currentAnim == anim::FALL;
    const bool landing = currentAnim == anim::JUMP_END || currentAnim == anim::JUMP_LAND_RUN;
    if (airborne) return inAirLoop || currentAnim == anim::JUMP_END ? AirborneAnimSync::Hold
                                                                    : AirborneAnimSync::Fall;
    if (landing) return AirborneAnimSync::Hold;
    return inAirLoop ? AirborneAnimSync::Refresh : AirborneAnimSync::None;
}

} // namespace rendering
} // namespace wowee
